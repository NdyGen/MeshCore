#include <gtest/gtest.h>

#include <helpers/rdm/MailboxCore.h>
#include <helpers/rdm/RdmChatMesh.h>
#include <helpers/rdm/RdmCodec.h>
#include <helpers/rdm/RdmConfig.h>
#include <helpers/rdm/RdmCrypto.h>
#include <helpers/rdm/RdmNode.h>

#include <MemFileIO.h>
#include <SimFileIO.h>

#include <type_traits>

using namespace rdm;

// Wire values are fixed by 03-ontwerp.md par. 3 and 06-implementatieplan-v1.md par. 3.
static_assert(TXT_TYPE_RDM_CTRL == 8 && (TXT_TYPE_RDM_CTRL << 2) == 0x20, "CTRL flags byte must be 0x20");
static_assert(CAP_BYTE == 0x81, "cap byte");
static_assert(REQ_DEPOSIT == 0x41 && REQ_FETCH == 0x42 && REQ_STATUS == 0x44 && REQ_RECEIPT_QUERY == 0x45, "req types");
static_assert(REG_MARKER0 == 0xFF && REG_MARKER1 == 'M', "registration marker");
static_assert(FETCH_FLAG_NO_PAYLOAD == 0x02, "fetch flags");
static_assert((FW_ATTEMPT_BASE & 3) == 0 && FW_ATTEMPT_BASE + 3 <= 255, "firmware attempts keep the ACK class");
static_assert(MAX_TEXT == 157 && MAX_TEXT_MAILBOX == 152 && MAX_INNER_PAYLOAD == 164 && MAX_BATCH == 8, "limits");
static_assert(4 + 1 + 4 + 1 + MAX_INNER_PAYLOAD <= 176, "DEPOSIT plaintext fits the exact datagram check");
static_assert(4 + 1 + 1 + 1 + MAX_INNER_PAYLOAD <= 176, "FETCH reply plaintext fits the exact datagram check");
static_assert(4 + ((5 + MAX_TEXT_MAILBOX + 3 + 15) / 16) * 16 == MAX_INNER_PAYLOAD, "mailbox text fills the inner payload");
static_assert(sizeof(OutState) == 1 && sizeof(UserStatus) == 1 && sizeof(MbxCode) == 1, "one-byte enums");
static_assert((uint8_t)UserStatus::NO_OUTBOX == 9 && (uint8_t)MbxCode::RATE_LIMITED == 0x15, "enum wire values");
static_assert((uint8_t)MbxState::SYNC_EXPIRED == 6 && (uint8_t)ReportResult::INBOX_FULL == 4, "enum wire values");
static_assert(std::is_abstract<RdmChatMesh>::value, "app side of NodeHost stays pure virtual");
static_assert(!std::is_abstract<Node>::value, "Node implements all host interfaces");
static_assert(!std::is_abstract<MemFileIO>::value && !std::is_abstract<SimFileIO>::value, "FileIO test doubles");

TEST(RdmTypes, UserStatusMapping) {
  EXPECT_EQ(toUserStatus(OutState::NEW), UserStatus::QUEUED);
  EXPECT_EQ(toUserStatus(OutState::WAIT_ACK), UserStatus::QUEUED);
  EXPECT_EQ(toUserStatus(OutState::RETRY), UserStatus::QUEUED);
  EXPECT_EQ(toUserStatus(OutState::DEPOSITING), UserStatus::QUEUED);
  EXPECT_EQ(toUserStatus(OutState::REGISTER), UserStatus::QUEUED);
  EXPECT_EQ(toUserStatus(OutState::CUSTODY), UserStatus::CUSTODY);
  EXPECT_EQ(toUserStatus(OutState::ON_RADIO), UserStatus::ON_RADIO);
  EXPECT_EQ(toUserStatus(OutState::ON_RADIO_FINAL), UserStatus::ON_RADIO_FINAL);
  EXPECT_EQ(toUserStatus(OutState::DELIVERED), UserStatus::DELIVERED);
  EXPECT_EQ(toUserStatus(OutState::REJECTED), UserStatus::REJECTED);
  EXPECT_EQ(toUserStatus(OutState::EXPIRED), UserStatus::EXPIRED);
  EXPECT_EQ(toUserStatus(OutState::SYNC_EXPIRED), UserStatus::SYNC_EXPIRED);
}

TEST(RdmTypes, FinalStates) {
  for (int s = 0; s <= (int)OutState::SYNC_EXPIRED; s++) {
    bool expect = s >= (int)OutState::ON_RADIO_FINAL;
    EXPECT_EQ(isFinal((OutState)s), expect) << "state " << s;
  }
}

TEST(MemFileIO, CreateReadWriteRemove) {
  MemFileIO io(4096);
  EXPECT_FALSE(io.exists("/rdm/a"));
  EXPECT_EQ(io.size("/rdm/a"), -1);
  ASSERT_TRUE(io.create("/rdm/a", 100));
  EXPECT_EQ(io.size("/rdm/a"), 100);
  EXPECT_EQ(io.freeBytes(), 4096u - 100u);

  const uint8_t data[4] = {1, 2, 3, 4};
  ASSERT_TRUE(io.write("/rdm/a", 10, data, 4));
  uint8_t back[4] = {};
  ASSERT_TRUE(io.read("/rdm/a", 10, back, 4));
  EXPECT_EQ(0, memcmp(data, back, 4));

  EXPECT_FALSE(io.write("/rdm/a", 98, data, 4)) << "in-place writes never grow a file";
  EXPECT_FALSE(io.read("/rdm/a", 98, back, 4));
  EXPECT_FALSE(io.write("/rdm/missing", 0, data, 4));

  ASSERT_TRUE(io.create("/rdm/a", 50)) << "create replaces";
  EXPECT_EQ(io.size("/rdm/a"), 50);
  ASSERT_TRUE(io.read("/rdm/a", 10, back, 4));
  EXPECT_EQ(back[0], 0) << "replaced file is zero-filled";

  EXPECT_TRUE(io.remove("/rdm/a"));
  EXPECT_FALSE(io.remove("/rdm/a"));
}

TEST(MemFileIO, CapacityLimitsCreate) {
  MemFileIO io(100);
  EXPECT_TRUE(io.create("/a", 60));
  EXPECT_FALSE(io.create("/b", 60));
  EXPECT_TRUE(io.create("/a", 100)) << "replacing counts only the new size";
  EXPECT_EQ(io.freeBytes(), 0u);
}

TEST(MemFileIO, TornWrite) {
  MemFileIO io;
  ASSERT_TRUE(io.create("/f", 8));
  const uint8_t data[8] = {9, 9, 9, 9, 9, 9, 9, 9};
  io.failWritesAfter(3);
  EXPECT_FALSE(io.write("/f", 0, data, 8));
  EXPECT_EQ(io.files["/f"][2], 9);
  EXPECT_EQ(io.files["/f"][3], 0) << "bytes past the budget never reach flash";
  EXPECT_FALSE(io.write("/f", 0, data, 1)) << "budget exhausted: every later write fails";
  EXPECT_FALSE(io.create("/g", 4));
  io.clearFaults();
  EXPECT_TRUE(io.write("/f", 0, data, 8));
}

TEST(MemFileIO, FailNextCreateAndCorrupt) {
  MemFileIO io;
  io.failNextCreate();
  EXPECT_FALSE(io.create("/f", 4));
  EXPECT_TRUE(io.create("/f", 4));
  EXPECT_TRUE(io.corrupt("/f", 1, 0xFF));
  EXPECT_EQ(io.files["/f"][1], 0xFF);
  EXPECT_FALSE(io.corrupt("/f", 4, 0xFF));
  io.wipeAll();
  EXPECT_FALSE(io.exists("/f"));
}

TEST(SimFileIO, UsesSimFlashAndTornWriteBudget) {
  sim::SimFS fs;
  fs.capacity_bytes = 64;
  SimFileIO io(fs);
  ASSERT_TRUE(io.create("/rdm/x", 16));
  EXPECT_TRUE(fs.exists("/rdm/x"));
  EXPECT_EQ(io.freeBytes(), 48u);
  EXPECT_FALSE(io.create("/rdm/y", 49));

  const uint8_t data[4] = {5, 6, 7, 8};
  fs.fail_writes_after(2);
  EXPECT_FALSE(io.write("/rdm/x", 0, data, 4));
  EXPECT_EQ(fs.files["/rdm/x"][1], 6);
  EXPECT_EQ(fs.files["/rdm/x"][2], 0);
  fs.write_budget = -1;
  EXPECT_TRUE(io.write("/rdm/x", 0, data, 4));
  uint8_t back[4];
  ASSERT_TRUE(io.read("/rdm/x", 0, back, 4));
  EXPECT_EQ(0, memcmp(data, back, 4));
  fs.wipe();
  EXPECT_EQ(io.size("/rdm/x"), -1);
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
