#include <gtest/gtest.h>

#include <RdmCompanionProto.h>

#include <string>
#include <vector>

using namespace rdm;
using namespace rdm::companion;

namespace {

typedef std::vector<uint8_t> Bytes;

const uint8_t PREFIX[6] = {0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6};
const uint8_t KEY[4] = {0x11, 0x22, 0x33, 0x44};

Bytes le32(uint32_t v) { return Bytes{(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)}; }

Bytes cat(std::initializer_list<Bytes> parts) {
  Bytes out;
  for (const Bytes& p : parts) out.insert(out.end(), p.begin(), p.end());
  return out;
}

Bytes prefix() { return Bytes(PREFIX, PREFIX + 6); }
Bytes key() { return Bytes(KEY, KEY + 4); }

StatusPush samplePush(UserStatus s) {
  StatusPush p;
  p.app_ack = 0xCAFEBABE;
  p.status = s;
  memcpy(p.key, KEY, 4);
  p.ts = 0x66F9A0B1;
  memcpy(p.pub_prefix, PREFIX, 6);
  return p;
}

OutEntry sampleEntry(OutState st) {
  OutEntry e;
  memset(&e, 0, sizeof(e));
  memcpy(e.pub_prefix, PREFIX, 6);
  e.ts = 0x66F9A0B1;
  memcpy(e.key, KEY, 4);
  e.state = st;
  e.app_ack = 0x01020304;
  e.text_len = 2;
  memcpy(e.text, "hi", 3);
  return e;
}

InRecord sampleRecord(const char* text) {
  InRecord r;
  memset(&r, 0, sizeof(r));
  memcpy(r.sender_prefix, PREFIX, 6);
  r.ts = 0x66F9A0B1;
  r.recv_time = 0x66F9A0C0;
  r.txt_type = 0;
  r.path_len = 2;
  r.snr_x4 = -7;
  r.text_len = (uint8_t)strlen(text);
  memcpy(r.text, text, r.text_len + 1);
  return r;
}

}

// ---- PUSH_CODE_RDM_STATUS (06 par. 3.14) ----

TEST(StatusPush, IsTwentyBytesInDocumentedOrder) {
  uint8_t f[STATUS_FRAME_LEN];
  ASSERT_EQ(encodeStatus(f, samplePush(UserStatus::CUSTODY)), 20u);
  Bytes expect = cat({{0x91}, le32(0xCAFEBABE), {(uint8_t)UserStatus::CUSTODY}, key(), le32(0x66F9A0B1), prefix()});
  EXPECT_EQ(Bytes(f, f + 20), expect);
}

TEST(StatusPush, RoundTripsEveryStatus) {
  for (uint8_t s = 0; s <= (uint8_t)UserStatus::NO_OUTBOX; s++) {
    uint8_t f[STATUS_FRAME_LEN];
    encodeStatus(f, samplePush((UserStatus)s));
    StatusPush back;
    ASSERT_TRUE(decodeStatus(f, sizeof(f), back));
    EXPECT_EQ((uint8_t)back.status, s);
    EXPECT_EQ(back.app_ack, 0xCAFEBABEu);
    EXPECT_EQ(back.ts, 0x66F9A0B1u);
    EXPECT_EQ(memcmp(back.key, KEY, 4), 0);
    EXPECT_EQ(memcmp(back.pub_prefix, PREFIX, 6), 0);
  }
}

TEST(StatusPush, DecodeRejectsOtherCodesShortFramesAndUnknownStatus) {
  uint8_t f[STATUS_FRAME_LEN];
  encodeStatus(f, samplePush(UserStatus::DELIVERED));
  StatusPush back;
  EXPECT_FALSE(decodeStatus(f, 19, back));
  f[0] = 0x82;
  EXPECT_FALSE(decodeStatus(f, 20, back));
  f[0] = PUSH_CODE_RDM_STATUS;
  f[5] = (uint8_t)UserStatus::NO_OUTBOX + 1;
  EXPECT_FALSE(decodeStatus(f, 20, back));
}

// ---- CMD_RDM_LIST_OUTBOX reply: START, ENTRY per entry, END (06 par. 3.14) ----

// Reply codes stay below 0x80, so clients that split replies from pushes by code see them as the command's reply.
TEST(OutboxList, CodesAreReplyCodes) {
  EXPECT_LT(RESP_CODE_RDM_OUTBOX_START, 0x80);
  EXPECT_LT(RESP_CODE_RDM_OUTBOX_ENTRY, 0x80);
  EXPECT_LT(RESP_CODE_RDM_OUTBOX_END, 0x80);
  EXPECT_GE(PUSH_CODE_RDM_STATUS, 0x80);
}

TEST(OutboxList, StartCarriesTheCount) {
  uint8_t f[OUTBOX_START_LEN];
  ASSERT_EQ(encodeOutboxStart(f, 5), 2u);
  EXPECT_EQ(Bytes(f, f + 2), (Bytes{0x40, 5}));
  uint8_t count = 0;
  ASSERT_TRUE(decodeOutboxStart(f, 2, count));
  EXPECT_EQ(count, 5);
  EXPECT_FALSE(decodeOutboxStart(f, 1, count));
  f[0] = RESP_CODE_RDM_OUTBOX_ENTRY;
  EXPECT_FALSE(decodeOutboxStart(f, 2, count));
}

TEST(OutboxList, EntryIsTwentyTwoBytesInDocumentedOrder) {
  uint8_t f[OUTBOX_ENTRY_LEN];
  ASSERT_EQ(encodeOutboxEntry(f, outboxRow(sampleEntry(OutState::ON_RADIO), 2)), 22u);
  Bytes expect = cat({{0x41, 2}, prefix(), le32(0x66F9A0B1), le32(0x01020304),
                      {(uint8_t)UserStatus::ON_RADIO, (uint8_t)OutState::ON_RADIO}, key()});
  EXPECT_EQ(Bytes(f, f + 22), expect);
}

TEST(OutboxList, EndIsOneByte) {
  uint8_t f[OUTBOX_END_LEN];
  ASSERT_EQ(encodeOutboxEnd(f), 1u);
  EXPECT_EQ(f[0], 0x42);
}

// G1: the user status of a row is derived from the outbox state, internal states read as QUEUED.
TEST(OutboxList, UserStatusFollowsG1Mapping) {
  const OutState queued[] = {OutState::NEW, OutState::WAIT_ACK, OutState::RETRY, OutState::DEPOSITING, OutState::REGISTER};
  for (OutState st : queued) EXPECT_EQ(outboxRow(sampleEntry(st), 0).status, UserStatus::QUEUED);
  EXPECT_EQ(outboxRow(sampleEntry(OutState::CUSTODY), 0).status, UserStatus::CUSTODY);
  EXPECT_EQ(outboxRow(sampleEntry(OutState::SYNC_EXPIRED), 0).status, UserStatus::SYNC_EXPIRED);
  EXPECT_EQ(outboxRow(sampleEntry(OutState::SYNC_EXPIRED), 0).state, OutState::SYNC_EXPIRED);
}

TEST(OutboxList, EntryRoundTripsAndRejectsTruncation) {
  uint8_t f[OUTBOX_ENTRY_LEN];
  encodeOutboxEntry(f, outboxRow(sampleEntry(OutState::DEPOSITING), 7));
  OutboxRow back;
  ASSERT_TRUE(decodeOutboxEntry(f, 22, back));
  EXPECT_EQ(back.idx, 7);
  EXPECT_EQ(back.ts, 0x66F9A0B1u);
  EXPECT_EQ(back.app_ack, 0x01020304u);
  EXPECT_EQ(back.status, UserStatus::QUEUED);
  EXPECT_EQ(back.state, OutState::DEPOSITING);
  EXPECT_EQ(memcmp(back.key, KEY, 4), 0);
  EXPECT_EQ(memcmp(back.pub_prefix, PREFIX, 6), 0);
  for (size_t n = 0; n < 22; n++) EXPECT_FALSE(decodeOutboxEntry(f, n, back)) << n;
}

// ---- commands ----

TEST(Commands, EnableCarriesProtocolVersion) {
  uint8_t f[2];
  ASSERT_EQ(buildEnable(f), 2u);
  EXPECT_EQ(Bytes(f, f + 2), (Bytes{0xC0, 1}));
  uint8_t ver = 0;
  ASSERT_TRUE(parseEnable(f, 2, ver));
  EXPECT_EQ(ver, PROTO_VERSION);
  EXPECT_FALSE(parseEnable(f, 1, ver));
}

TEST(Commands, SetMailboxWithKeysOrEmptyToClear) {
  uint8_t pub[32], k[16];
  for (int i = 0; i < 32; i++) pub[i] = (uint8_t)(i + 1);
  for (int i = 0; i < 16; i++) k[i] = (uint8_t)(0x80 + i);
  uint8_t f[SET_MAILBOX_LEN];
  ASSERT_EQ(buildSetMailbox(f, pub, k), 49u);
  EXPECT_EQ(f[0], CMD_RDM_SET_MAILBOX);
  const uint8_t *p = nullptr, *kk = nullptr;
  ASSERT_EQ(parseSetMailbox(f, 49, p, kk), MailboxCmd::SET);
  EXPECT_EQ(memcmp(p, pub, 32), 0);
  EXPECT_EQ(memcmp(kk, k, 16), 0);

  ASSERT_EQ(buildSetMailbox(f, nullptr, nullptr), 1u);
  EXPECT_EQ(parseSetMailbox(f, 1, p, kk), MailboxCmd::CLEAR);
  EXPECT_EQ(p, nullptr);
  EXPECT_EQ(kk, nullptr);

  EXPECT_EQ(parseSetMailbox(f, 33, p, kk), MailboxCmd::INVALID);
  EXPECT_EQ(parseSetMailbox(f, 48, p, kk), MailboxCmd::INVALID);
  EXPECT_EQ(parseSetMailbox(f, 50, p, kk), MailboxCmd::INVALID);
}

TEST(Commands, ListOutboxIsASingleByte) {
  uint8_t f[1];
  ASSERT_EQ(buildListOutbox(f), 1u);
  EXPECT_EQ(f[0], CMD_RDM_LIST_OUTBOX);
}

// ---- frames the standard app already knows ----

// Same bytes as MyMesh::queueMessage for a DM, so an inbox record is indistinguishable from an upstream DM.
TEST(ContactMsg, V3LayoutMatchesUpstreamQueueMessage) {
  uint8_t f[MAX_APP_FRAME];
  size_t n = encodeContactMsg(f, sizeof(f), sampleRecord("hello"), 3);
  Bytes expect = cat({{RESP_CODE_CONTACT_MSG_RECV_V3, (uint8_t)(int8_t)-7, 0, 0}, prefix(), {2, 0}, le32(0x66F9A0B1),
                      Bytes{'h', 'e', 'l', 'l', 'o'}});
  EXPECT_EQ(Bytes(f, f + n), expect);
}

TEST(ContactMsg, OldAppsGetTheV2Layout) {
  uint8_t f[MAX_APP_FRAME];
  InRecord r = sampleRecord("yo");
  r.path_len = 0xFF;   // direct
  size_t n = encodeContactMsg(f, sizeof(f), r, 2);
  Bytes expect = cat({{RESP_CODE_CONTACT_MSG_RECV}, prefix(), {0xFF, 0}, le32(0x66F9A0B1), Bytes{'y', 'o'}});
  EXPECT_EQ(Bytes(f, f + n), expect);
}

TEST(ContactMsg, TextIsCutAtTheFrameLimitLikeUpstream) {
  std::string text(INBOX_TEXT_MAX, 'x');
  uint8_t f[MAX_APP_FRAME];
  size_t n = encodeContactMsg(f, sizeof(f), sampleRecord(text.c_str()), 3);
  EXPECT_EQ(n, 16u + INBOX_TEXT_MAX);
  EXPECT_EQ(encodeContactMsg(f, 16 + 10, sampleRecord(text.c_str()), 3), 26u);
}

TEST(SendConfirmed, CarriesAppAckAndTripTime) {
  uint8_t f[SEND_CONFIRMED_LEN];
  ASSERT_EQ(encodeSendConfirmed(f, 0xCAFEBABE, 1234), 9u);
  EXPECT_EQ(Bytes(f, f + 9), cat({{0x82}, le32(0xCAFEBABE), le32(1234)}));
}

// ---- status channel (RDM_STATUS_CHANNEL) ----

TEST(StatusChannel, LineNamesContactStatusAndSendTime) {
  char line[64];
  size_t n = formatStatusLine(line, sizeof(line), "Alice", UserStatus::DELIVERED, 1727568000 + 13 * 3600 + 5 * 60);
  EXPECT_STREQ(line, "Alice: delivered (sent 13:05 UTC)");
  EXPECT_EQ(n, strlen(line));
}

TEST(StatusChannel, EveryStatusHasAName) {
  for (uint8_t s = 0; s <= (uint8_t)UserStatus::NO_OUTBOX; s++) {
    const char* name = statusName((UserStatus)s);
    ASSERT_NE(name, nullptr);
    EXPECT_GT(strlen(name), 0u) << (int)s;
  }
}

TEST(StatusChannel, LineIsTruncatedAndTerminated) {
  char line[12];
  size_t n = formatStatusLine(line, sizeof(line), "Alice", UserStatus::CUSTODY, 0);
  EXPECT_EQ(n, 11u);
  EXPECT_EQ(line[11], 0);
  EXPECT_EQ(std::string(line), "Alice: in m");
}

TEST(StatusChannel, ChannelFrameMatchesUpstreamChannelMessage) {
  uint8_t f[MAX_APP_FRAME];
  size_t n = encodeChannelMsg(f, sizeof(f), 3, 4, 0x66F9A0B1, "Bo: on radio");
  Bytes expect = cat({{RESP_CODE_CHANNEL_MSG_RECV_V3, 0, 0, 0, 4, 0xFF, 0}, le32(0x66F9A0B1)});
  std::string t = "Bo: on radio";
  expect.insert(expect.end(), t.begin(), t.end());
  EXPECT_EQ(Bytes(f, f + n), expect);

  n = encodeChannelMsg(f, sizeof(f), 2, 4, 0x66F9A0B1, "x");
  EXPECT_EQ(Bytes(f, f + n), cat({{RESP_CODE_CHANNEL_MSG_RECV, 4, 0xFF, 0}, le32(0x66F9A0B1), {'x'}}));
}

TEST(StatusChannel, SecretIsPerDeviceAnd128Bit) {
  uint8_t a[32] = {1}, b[32] = {2};
  uint8_t sa[32], sb[32], sa2[32];
  statusChannelSecret(sa, a);
  statusChannelSecret(sa2, a);
  statusChannelSecret(sb, b);
  EXPECT_EQ(memcmp(sa, sa2, 32), 0);
  EXPECT_NE(memcmp(sa, sb, 16), 0);
  const uint8_t zero[16] = {0};
  EXPECT_EQ(memcmp(&sa[16], zero, 16), 0);   // setChannel treats a zero upper half as a 128-bit key
  EXPECT_NE(memcmp(sa, zero, 16), 0);
}

// ---- outgoing frame queue ----

TEST(FrameQueue, IsFifoAndBounded) {
  FrameQueue<3, 24> q;
  EXPECT_TRUE(q.empty());
  const uint8_t a[] = {1}, b[] = {2, 2}, c[] = {3, 3, 3}, d[] = {4};
  EXPECT_TRUE(q.push(a, 1));
  EXPECT_TRUE(q.push(b, 2));
  EXPECT_TRUE(q.push(c, 3));
  EXPECT_FALSE(q.push(d, 1));
  EXPECT_EQ(q.size(), 3);
  size_t len;
  EXPECT_EQ(Bytes(q.front(len), q.front(len) + len), (Bytes{1}));
  q.pop();
  EXPECT_TRUE(q.push(d, 1));
  const uint8_t expect[][3] = {{2, 2}, {3, 3, 3}, {4}};
  const size_t lens[] = {2, 3, 1};
  for (int i = 0; i < 3; i++) {
    const uint8_t* p = q.front(len);
    ASSERT_EQ(len, lens[i]);
    EXPECT_EQ(memcmp(p, expect[i], len), 0);
    q.pop();
  }
  EXPECT_TRUE(q.empty());
}

TEST(FrameQueue, RejectsOversizedFramesAndClears) {
  FrameQueue<2, 4> q;
  const uint8_t big[5] = {0};
  EXPECT_FALSE(q.push(big, 5));
  EXPECT_FALSE(q.push(big, 0));
  EXPECT_TRUE(q.push(big, 4));
  q.clear();
  EXPECT_TRUE(q.empty());
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
