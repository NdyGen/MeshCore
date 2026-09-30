#include <gtest/gtest.h>

#include <helpers/rdm/RdmNode.h>

#include <MemFileIO.h>

#include <memory>
#include <string.h>

// Slot counts Node::begin() derives from the flash budget, and how files already on flash affect them. Measured on
// the planning code before D06; the table is the contract for the refactoring.

namespace {

struct MeshStub : rdm::NodeMeshHost {
  uint32_t millis() override { return 0; }
  uint32_t random32() override { return 1; }
  bool     txIdle() override { return true; }
  void     selfPub(uint8_t pub_out[32]) override { memset(pub_out, 1, 32); }
  bool lookupContact(const uint8_t*, uint8_t*, bool&, bool&) override { return false; }
  bool sendTxtPlain(const uint8_t*, const uint8_t*, size_t, bool, uint32_t&) override { return false; }
  bool sendAck(const uint8_t*, const uint8_t*, uint8_t) override { return false; }
  bool sendReq(const uint8_t*, uint32_t, const uint8_t*, size_t, bool, uint32_t&) override { return false; }
  bool sendAnonReq(const uint8_t*, const uint8_t*, size_t, uint32_t&) override { return false; }
  bool encryptTxtPayload(const uint8_t*, const uint8_t*, size_t, uint8_t*, size_t&) override { return false; }
  bool decryptTxtPayload(const uint8_t*, size_t, uint8_t*, uint8_t*, size_t&) override { return false; }
};

struct AppStub : rdm::NodeAppSink {
  uint32_t rtcNow(bool& trusted) override { trusted = false; return 0; }
  bool pushUserStatus(const uint8_t*, uint32_t, rdm::UserStatus, const uint8_t*, uint32_t) override { return true; }
  bool pushSendConfirmed(uint32_t) override { return true; }
  void pushMsgWaiting() override {}
};

struct Slots { uint16_t outbox, watch, contacts, inbox, reg; };

bool operator==(const Slots& a, const Slots& b) {
  return a.outbox == b.outbox && a.watch == b.watch && a.contacts == b.contacts && a.inbox == b.inbox && a.reg == b.reg;
}

std::ostream& operator<<(std::ostream& os, const Slots& s) {
  return os << "{" << s.outbox << ", " << s.watch << ", " << s.contacts << ", " << s.inbox << ", " << s.reg << "}";
}

uint16_t slotsOf(MemFileIO& io, const char* path) {
  auto it = io.files.find(path);
  if (it == io.files.end() || it->second.size() < 16) return 0;
  return (uint16_t)(it->second[8] | (it->second[9] << 8));
}

Slots slotsOf(MemFileIO& io) {
  return {slotsOf(io, rdm::Outbox::PATH), slotsOf(io, rdm::Outbox::WATCH_PATH), slotsOf(io, rdm::ContactTable::PATH),
          slotsOf(io, rdm::Inbox::PATH), slotsOf(io, rdm::Inbox::REG_PATH)};
}

bool boot(MemFileIO& io) {
  MeshStub mesh;
  AppStub app;
  std::unique_ptr<rdm::Node> node(new rdm::Node(io, mesh, app));
  return node->begin();
}

struct BudgetCase { uint32_t budget; Slots slots; };

const Slots MAX = {RDM_OUTBOX_SLOTS_MAX, RDM_WATCH_SLOTS_MAX, RDM_CONTACT_SLOTS_MAX, RDM_INBOX_SLOTS_MAX, RDM_REGISTER_SLOTS_MAX};

// budget = free flash plus the RDM files already there; the files take at most half of it (03 par. 6)
const BudgetCase BUDGETS[] = {
  {RDM_MIN_FREE_BYTES, {4, 9, 8, 18, 36}},     // the minima for outbox and contacts
  {32768, {4, 18, 9, 36, 73}},
  {65536, {4, 36, 18, 73, 146}},
  {100000, {6, 55, 27, 111, 223}},
  {131072, {9, 73, 36, 146, 293}},
  {200000, {13, 111, 55, 223, 447}},
  {228944, MAX},                               // twice the full plan: everything at its maximum
  {1u << 20, MAX},
};

}

TEST(RdmStoragePlan, SlotCountsFollowTheBudget) {
  for (const BudgetCase& c : BUDGETS) {
    MemFileIO io(c.budget);
    ASSERT_TRUE(boot(io)) << c.budget;
    EXPECT_EQ(slotsOf(io), c.slots) << "budget " << c.budget;
    EXPECT_EQ(slotsOf(io, rdm::Clock::PATH), 1) << c.budget;
    EXPECT_EQ(slotsOf(io, rdm::Node::MBX_PATH), 1) << c.budget;
    if (c.budget >= 65536) EXPECT_LE(io.usedBytes(), c.budget / 2) << "at most half of the budget once above the minima";
  }
}

TEST(RdmStoragePlan, BelowTheMinimumRdmStaysOff) {
  MemFileIO io(RDM_MIN_FREE_BYTES - 1);
  EXPECT_FALSE(boot(io));
  EXPECT_TRUE(io.files.empty());
}

// Files already on flash keep their slot count, whatever the budget says now (G7: other data growing never
// migrates records away), and their bytes count as budget.
TEST(RdmStoragePlan, ExistingFilesKeepTheirSlotCount) {
  MemFileIO io(1u << 20);
  ASSERT_TRUE(boot(io));
  ASSERT_EQ(slotsOf(io), MAX);
  io.setCapacity(io.usedBytes() + 32768);   // the same flash, most of it now taken by other data
  ASSERT_TRUE(boot(io));
  EXPECT_EQ(slotsOf(io), MAX);

  MemFileIO small(32768);
  ASSERT_TRUE(boot(small));
  ASSERT_EQ(slotsOf(small), (Slots{4, 18, 9, 36, 73}));
  small.setCapacity(1u << 20);
  ASSERT_TRUE(boot(small));
  EXPECT_EQ(slotsOf(small), (Slots{4, 18, 9, 36, 73})) << "more room later does not grow the files";
}

TEST(RdmStoragePlan, ExistingFileAboveTheMaximumIsCapped) {
  MemFileIO io(1u << 20);
  {
    rdm::RecordFile f(io, rdm::Outbox::PATH, 1, rdm::Outbox::RECORD_SIZE, RDM_OUTBOX_SLOTS_MAX + 4, true);
    ASSERT_EQ(f.open(), rdm::RecordFile::Open::CREATED);
  }
  ASSERT_TRUE(boot(io));
  EXPECT_EQ(slotsOf(io, rdm::Outbox::PATH), RDM_OUTBOX_SLOTS_MAX);
}

TEST(RdmStoragePlan, ExistingFileWithAnotherRecordSizeIsPlannedAfresh) {
  MemFileIO io(32768);
  {
    rdm::RecordFile f(io, rdm::Inbox::PATH, 1, rdm::Inbox::RECORD_SIZE + 1, 3, false);
    ASSERT_EQ(f.open(), rdm::RecordFile::Open::CREATED);
  }
  ASSERT_TRUE(boot(io));
  EXPECT_EQ(slotsOf(io, rdm::Inbox::PATH), 36) << "the scaled count for this budget, not the file's 3";
}

TEST(RdmStoragePlan, ExistingFilesCountAsBudget) {
  MemFileIO io(RDM_MIN_FREE_BYTES);
  ASSERT_TRUE(boot(io));
  uint32_t used = io.usedBytes();
  ASSERT_GT(used, 0u);
  io.setCapacity(RDM_MIN_FREE_BYTES);   // free = minimum - used, plus the files = the minimum again
  EXPECT_TRUE(boot(io));
  io.setCapacity(RDM_MIN_FREE_BYTES - 1);
  EXPECT_FALSE(boot(io));
}
