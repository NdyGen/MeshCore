#include <gtest/gtest.h>

#include <helpers/rdm/RdmBytes.h>
#include <helpers/rdm/RdmClock.h>
#include <helpers/rdm/RdmCodec.h>
#include <helpers/rdm/RdmCrypto.h>
#include <helpers/rdm/RdmFetcher.h>
#include <helpers/rdm/RdmNode.h>
#include <helpers/rdm/RdmPolicy.h>

#include <CompanionFrames.h>
#include <MemFileIO.h>
#include <RdmTestStack.h>
#include <TestUtil.h>

#include <vector>

using namespace rdm;

namespace {

// ---- RdmBytes.h ----

TEST(RdmBytes, LittleEndianLayout) {
  uint8_t b[4] = {0};
  put16(b, 0x1234);
  EXPECT_EQ(b[0], 0x34);
  EXPECT_EQ(b[1], 0x12);
  put32(b, 0x11223344);
  EXPECT_EQ(b[0], 0x44);
  EXPECT_EQ(b[1], 0x33);
  EXPECT_EQ(b[2], 0x22);
  EXPECT_EQ(b[3], 0x11);
}

TEST(RdmBytes, RoundTripAndHighBits) {
  const uint16_t v16[] = {0, 1, 0x00FF, 0x0100, 0x7FFF, 0x8000, 0xFFFF};
  for (uint16_t v : v16) {
    uint8_t b[2];
    put16(b, v);
    EXPECT_EQ(get16(b), v);
  }
  const uint32_t v32[] = {0, 1, 0xFF, 0x100, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu, 0xDEADBEEFu};
  for (uint32_t v : v32) {
    uint8_t b[4];
    put32(b, v);
    EXPECT_EQ(get32(b), v);
  }
  const uint8_t ff[4] = {0xFF, 0xFF, 0xFF, 0xFF};
  EXPECT_EQ(get16(ff), 0xFFFF) << "no sign extension";
  EXPECT_EQ(get32(ff), 0xFFFFFFFFu);
}

TEST(RdmBytes, WritesOnlyItsOwnBytes) {
  uint8_t b[6] = {0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA};
  put32(b + 1, 0);
  EXPECT_EQ(b[0], 0xAA);
  EXPECT_EQ(b[5], 0xAA);
  put16(b + 1, 0);
  EXPECT_EQ(b[3], 0x00);
}

TEST(RdmBytes, IsZero) {
  EXPECT_TRUE(isZero(nullptr, 0)) << "length 0 reads nothing";
  const uint8_t z[8] = {0};
  EXPECT_TRUE(isZero(z, sizeof(z)));
  uint8_t last[8] = {0};
  last[7] = 1;
  EXPECT_FALSE(isZero(last, sizeof(last)));
  EXPECT_TRUE(isZero(last, 7)) << "stops at n";
  const uint8_t first[1] = {0x80};
  EXPECT_FALSE(isZero(first, 1));
}

// ---- RdmPolicy.h, against the formulas of 03 (G3, G16, G17) written out here ----

uint32_t refJitterMax(uint32_t interval) {
  uint32_t by_div = interval / RDM_JITTER_DIV;
  return by_div < (uint32_t)RDM_JITTER_MAX_S ? by_div : (uint32_t)RDM_JITTER_MAX_S;
}

TEST(RdmPolicy, JitterMaxBounds) {
  EXPECT_EQ(jitterMax(0), 0u);
  EXPECT_EQ(jitterMax(RDM_JITTER_DIV - 1), 0u);
  EXPECT_EQ(jitterMax(RDM_JITTER_DIV), 1u);
  const uint32_t knee = RDM_JITTER_MAX_S * RDM_JITTER_DIV;   // 600 s: I / DIV reaches the cap
  EXPECT_EQ(jitterMax(knee - 1), (uint32_t)RDM_JITTER_MAX_S - 1);
  EXPECT_EQ(jitterMax(knee), (uint32_t)RDM_JITTER_MAX_S);
  EXPECT_EQ(jitterMax(knee + 1), (uint32_t)RDM_JITTER_MAX_S);
  EXPECT_EQ(jitterMax(RDM_FETCH_INTERVAL_S), (uint32_t)RDM_JITTER_MAX_S);
  EXPECT_EQ(jitterMax(UINT32_MAX), (uint32_t)RDM_JITTER_MAX_S);
  for (uint32_t i = 0; i < 2000; i++) EXPECT_EQ(jitterMax(i), refJitterMax(i)) << i;
}

TEST(RdmPolicy, JitteredStaysInRangeAndUsesOnlyTheGivenRandom) {
  const uint32_t intervals[] = {0, 5, 300, 600, 900, 3600, 86400};
  const uint32_t rnds[] = {0, 1, 59, 60, 61, 12345, 0x7FFFFFFFu, 0xFFFFFFFFu};
  for (uint32_t I : intervals) {
    for (uint32_t r : rnds) {
      uint32_t j = jittered(I, r);
      EXPECT_GE(j, I);
      EXPECT_LE(j, I + refJitterMax(I));
      EXPECT_EQ(j, I + r % (refJitterMax(I) + 1)) << I << " " << r;
      EXPECT_EQ(jittered(I, r), j) << "pure function";
    }
    EXPECT_EQ(jittered(I, 0), I);
    EXPECT_EQ(jittered(I, refJitterMax(I)), I + refJitterMax(I)) << "upper bound reachable";
  }
}

TEST(RdmPolicy, FirstRetryDelayRange) {
  EXPECT_EQ(firstRetryDelay(0), (uint32_t)RDM_RETRY_FIRST_MIN_S);
  const uint32_t span = RDM_RETRY_FIRST_MAX_S - RDM_RETRY_FIRST_MIN_S + 1;
  EXPECT_EQ(firstRetryDelay(span - 1), (uint32_t)RDM_RETRY_FIRST_MAX_S);
  EXPECT_EQ(firstRetryDelay(span), (uint32_t)RDM_RETRY_FIRST_MIN_S);
  bool seen_min = false, seen_max = false;
  Lcg32 g{42};
  for (int i = 0; i < 5000; i++) {
    uint32_t r = g.next();
    uint32_t d = firstRetryDelay(r);
    EXPECT_GE(d, (uint32_t)RDM_RETRY_FIRST_MIN_S);
    EXPECT_LE(d, (uint32_t)RDM_RETRY_FIRST_MAX_S);
    EXPECT_EQ(d, RDM_RETRY_FIRST_MIN_S + r % span);
    seen_min |= d == RDM_RETRY_FIRST_MIN_S;
    seen_max |= d == RDM_RETRY_FIRST_MAX_S;
  }
  EXPECT_TRUE(seen_min && seen_max);
  EXPECT_LE(firstRetryDelay(UINT32_MAX), (uint32_t)RDM_RETRY_FIRST_MAX_S);
}

TEST(RdmPolicy, WaitSecsAroundThirtySeconds) {
  EXPECT_EQ(waitSecs(0), (uint32_t)RDM_WAIT_ACK_MIN_S);
  EXPECT_EQ(waitSecs(14999), 30u);
  EXPECT_EQ(waitSecs(15000), 30u) << "2 x 15 s is exactly the minimum";
  EXPECT_EQ(waitSecs(15001), 31u) << "rounded up to whole seconds";
  EXPECT_EQ(waitSecs(15499), 31u);
  EXPECT_EQ(waitSecs(15500), 31u);
  EXPECT_EQ(waitSecs(15501), 32u);
  EXPECT_EQ(waitSecs(60000), 120u);
  EXPECT_EQ(waitSecs(UINT32_MAX), (uint32_t)((2ull * UINT32_MAX + 999) / 1000)) << "no 32-bit overflow";
}

// ---- RdmTypes.h ----

TEST(RdmTypes, SameReportComparesEveryField) {
  Report a;
  memset(&a, 0, sizeof(a));
  fillPattern(a.pkt_hash, 8, 1);
  fillPattern(a.ack, 6, 9);
  a.result = ReportResult::SYNCED;
  Report b = a;
  EXPECT_TRUE(sameReport(a, b));
  b.result = ReportResult::ON_RADIO;
  EXPECT_FALSE(sameReport(a, b));
  b = a;
  b.pkt_hash[7] ^= 1;
  EXPECT_FALSE(sameReport(a, b));
  b = a;
  b.ack[5] ^= 1;
  EXPECT_FALSE(sameReport(a, b));
}

// ---- record sizes and paths: the on-flash format ----

static_assert(Outbox::RECORD_SIZE == 201 && Outbox::WATCH_RECORD_SIZE == 28, "outbox record sizes");
static_assert(Inbox::RECORD_SIZE == 188 && Inbox::REG_RECORD_SIZE == 36, "inbox record sizes");
static_assert(ContactTable::RECORD_SIZE == 52 && Clock::RECORD_SIZE == 12, "contact and meta record sizes");
static_assert(Node::MBX_RECORD_SIZE == 32 + 16, "own mailbox record: pubkey | K_owner");

TEST(RdmRecords, PathsAreUnchanged) {
  EXPECT_STREQ(Clock::PATH, "/rdm/meta");
  EXPECT_STREQ(ContactTable::PATH, "/rdm/contacts");
  EXPECT_STREQ(Outbox::PATH, "/rdm/outbox");
  EXPECT_STREQ(Outbox::WATCH_PATH, "/rdm/watch");
  EXPECT_STREQ(Inbox::PATH, "/rdm/inbox");
  EXPECT_STREQ(Inbox::REG_PATH, "/rdm/register");
  EXPECT_STREQ(Node::MBX_PATH, "/rdm/mbx");
}

// ---- CTRL plaintext through the codec ----

codec::MbxInfo mbxInfo(uint8_t sub) {
  codec::MbxInfo info;
  memset(&info, 0, sizeof(info));
  info.sub = sub;
  info.version = CTRL_VERSION;
  if (sub == CTRL_MBX_INFO) {
    fillPattern(info.mbx_pub, 32, 3);
    fillPattern(info.token, 8, 200);
  }
  return info;
}

// Node::onCtrlTxt used to hash the first 47 or 7 received bytes; it now hashes buildCtrlPlain of the parsed fields.
TEST(RdmCtrlPlain, RebuildAfterParseEqualsReceivedBytes) {
  const uint8_t subs[] = {CTRL_MBX_INFO, CTRL_MBX_REVOKE};
  const uint32_t times[] = {0, 1, 0x80000000u, 0xFFFFFFFFu, 1790640000u};
  TestKey sender(77);
  for (uint8_t sub : subs) {
    for (uint32_t ts : times) {
      uint8_t sent[64];
      memset(sent, 0, sizeof(sent));
      size_t n = codec::buildCtrlPlain(sent, ts, mbxInfo(sub));
      ASSERT_EQ(n, sub == CTRL_MBX_INFO ? codec::CTRL_INFO_LEN : codec::CTRL_REVOKE_LEN);
      size_t padded = (n + 15) / 16 * 16;   // what decryption hands over
      uint32_t got_ts;
      codec::MbxInfo got;
      ASSERT_TRUE(codec::parseCtrlPlain(sent, padded, got_ts, got));
      uint8_t rebuilt[codec::CTRL_INFO_LEN];
      size_t m = codec::buildCtrlPlain(rebuilt, got_ts, got);
      size_t old_len = got.sub == CTRL_MBX_INFO ? 47 : 7;
      ASSERT_EQ(m, old_len);
      EXPECT_EQ(memcmp(rebuilt, sent, m), 0) << "sub " << (int)sub << " ts " << ts;
      uint8_t ack_old[4], ack_new[4];
      crypto::ctrlAck(ack_old, sent, old_len, sender.pub);
      crypto::ctrlAck(ack_new, rebuilt, m, sender.pub);
      EXPECT_EQ(memcmp(ack_old, ack_new, 4), 0);
    }
  }
}

// Outbox::computeAcks wrote ts with put32, Node::sendDm with memcpy of the host value: both are ctrlPlainFromBody.
TEST(RdmCtrlPlain, FromBodyEqualsTheFormerInlineBuilds) {
  const uint8_t subs[] = {CTRL_MBX_INFO, CTRL_MBX_REVOKE};
  for (uint8_t sub : subs) {
    uint32_t ts = 0xA1B2C3D4u;
    uint8_t full[codec::CTRL_INFO_LEN];
    size_t n = codec::buildCtrlPlain(full, ts, mbxInfo(sub));
    const uint8_t* body = full + 5;   // what Outbox stores: sub onwards
    size_t body_len = n - 5;

    uint8_t out[5 + MAX_TEXT];
    ASSERT_EQ(codec::ctrlPlainFromBody(out, ts, body, body_len), n);
    EXPECT_EQ(memcmp(out, full, n), 0);

    uint8_t outbox_old[5 + MAX_TEXT];
    outbox_old[0] = (uint8_t)ts;
    outbox_old[1] = (uint8_t)(ts >> 8);
    outbox_old[2] = (uint8_t)(ts >> 16);
    outbox_old[3] = (uint8_t)(ts >> 24);
    outbox_old[4] = TXT_TYPE_RDM_CTRL << 2;
    memcpy(outbox_old + 5, body, body_len);
    EXPECT_EQ(memcmp(out, outbox_old, n), 0);

    uint8_t node_old[5 + MAX_TEXT + 3];
    memcpy(node_old, &ts, 4);
    node_old[4] = TXT_TYPE_RDM_CTRL << 2;
    memcpy(node_old + 5, body, body_len);
    EXPECT_EQ(memcmp(out, node_old, n), 0) << "all RDM targets are little-endian";
  }
}

TEST(RdmCtrlPlain, FromBodyLimits) {
  uint8_t body[MAX_TEXT + 1];
  memset(body, 0x11, sizeof(body));
  uint8_t out[5 + MAX_TEXT + 1];
  EXPECT_EQ(codec::ctrlPlainFromBody(out, 1, body, 0), 5u);
  EXPECT_EQ(codec::ctrlPlainFromBody(out, 1, body, MAX_TEXT), 5 + MAX_TEXT);
  EXPECT_EQ(codec::ctrlPlainFromBody(out, 1, body, MAX_TEXT + 1), 0u) << "longer than an outbox entry holds";
}

// ---- Fetcher skips building a FETCH its host would refuse (FetcherHost::canFetch) ----

class GateRig : public InboxHost, public FetcherHost {
public:
  bool allowed = true;
  int asked = 0;
  std::vector<uint32_t> sent_at;
  uint32_t now = 1000;

  void onInboxChanged() override {}
  bool sendAckS(const uint8_t*, const uint8_t*) override { return true; }
  void onReportQueued() override {}
  uint32_t random32() override { return 0; }
  bool ownMailbox(uint8_t mbx_pub_out[32]) override {
    memset(mbx_pub_out, 0x4D, 32);
    return true;
  }
  bool canFetch() override {
    asked++;
    return allowed;
  }
  bool sendFetch(uint8_t, uint32_t, const Report*, uint8_t, uint32_t& est_timeout_ms) override {
    EXPECT_TRUE(allowed) << "sendFetch after canFetch() said no";
    sent_at.push_back(now);
    est_timeout_ms = 1000;
    return true;
  }
};

TEST(RdmFetcherGate, BlockedFetchStaysDueAndGoesOutOnceAllowed) {
  MemFileIO io;
  GateRig rig;
  RdmTestStack stack(io);
  stack.boot(rig);
  ASSERT_FALSE(HasFatalFailure());
  Fetcher fetcher(*stack.inbox, rig);
  fetcher.begin(rig.now, 7, 0);

  rig.allowed = false;
  for (uint32_t t = rig.now; t < rig.now + 10; t++) fetcher.loop(t);
  EXPECT_TRUE(rig.sent_at.empty());
  EXPECT_EQ(rig.asked, 10);
  EXPECT_EQ(fetcher.nextDue(rig.now + 10), rig.now + 10) << "still due";

  rig.allowed = true;
  rig.now += 10;
  fetcher.loop(rig.now);
  ASSERT_EQ(rig.sent_at.size(), 1u);
  EXPECT_EQ(rig.sent_at[0], rig.now);
}

TEST(RdmFetcherGate, DefaultHostAlwaysAllows) {
  struct Plain : FetcherHost {
    bool ownMailbox(uint8_t*) override { return true; }
    bool sendFetch(uint8_t, uint32_t, const Report*, uint8_t, uint32_t&) override { return true; }
    uint32_t random32() override { return 0; }
  } host;
  EXPECT_TRUE(host.canFetch());
}

// ---- test helpers ----

TEST(TestUtil, FillPatternReproducesTheOldGenerators) {
  for (uint8_t seed = 0; seed < 8; seed++) {
    uint8_t got[40], want[40];
    fillPattern(got, sizeof(got), seed);
    for (size_t i = 0; i < sizeof(want); i++) want[i] = (uint8_t)(seed + i * 7);
    EXPECT_EQ(memcmp(got, want, sizeof(got)), 0) << "seed + i * 7";

    fillPattern(got, 32, seed * 13, 1);
    for (int i = 0; i < 32; i++) want[i] = (uint8_t)(seed * 13 + i);
    EXPECT_EQ(memcmp(got, want, 32), 0) << "seed * 13 + i";

    fillPattern(got, 32, seed * 31, 7, 1);
    for (int i = 0; i < 32; i++) want[i] = (uint8_t)(seed * 31 + i * 7 + 1);
    EXPECT_EQ(memcmp(got, want, 32), 0) << "seed * 31 + i * 7 + 1";

    fillPattern(got, 40, seed * 31, 1);
    for (int i = 0; i < 40; i++) want[i] = (uint8_t)(seed * 31 + i);
    EXPECT_EQ(memcmp(got, want, 40), 0) << "seed * 31 + i";
  }
  TestKey k(5);
  for (int i = 0; i < 32; i++) EXPECT_EQ(k.pub[i], (uint8_t)(5 + i * 7));
}

TEST(TestUtil, Hex) {
  const uint8_t b[] = {0x00, 0x0F, 0xA0, 0xFF, 0x5C};
  EXPECT_EQ(toHexLower(b, sizeof(b)), "000fa0ff5c");
  EXPECT_EQ(toHexLower(std::vector<uint8_t>()), "");
  std::vector<uint8_t> v;
  ASSERT_TRUE(fromHex("000fa0ff5c", v));
  EXPECT_EQ(v, std::vector<uint8_t>(b, b + sizeof(b)));
  ASSERT_TRUE(fromHex("000FA0FF5C", v)) << "upper case accepted";
  EXPECT_EQ(v, std::vector<uint8_t>(b, b + sizeof(b)));
  ASSERT_TRUE(fromHex("", v));
  EXPECT_TRUE(v.empty());
  v = {1};
  EXPECT_FALSE(fromHex("abc", v)) << "odd length";
  EXPECT_FALSE(fromHex("0g", v)) << "non-hex";
  EXPECT_FALSE(fromHex(" f", v));
  EXPECT_EQ(v, std::vector<uint8_t>{1}) << "untouched on failure";
  uint8_t fixed[2];
  EXPECT_TRUE(fromHex("beef", fixed, 2));
  EXPECT_EQ(fixed[0], 0xBE);
  EXPECT_EQ(fixed[1], 0xEF);
  EXPECT_FALSE(fromHex("beef", fixed, 3)) << "wrong length";
}

TEST(TestUtil, Lcg32KeepsTheOldSequences) {
  Lcg32 nr{123};
  uint32_t s = 123;
  for (int i = 0; i < 100; i++) {
    s = s * 1664525u + 1013904223u;
    EXPECT_EQ(nr.next(), s);
  }
  Lcg32 ansi{7, 1103515245u, 12345u};
  s = 7;
  for (int i = 0; i < 100; i++) {
    s = s * 1103515245u + 12345u;
    EXPECT_EQ(ansi.next(), s);
  }
}

struct NullInboxHost : InboxHost {
  void onInboxChanged() override {}
  bool sendAckS(const uint8_t*, const uint8_t*) override { return true; }
  void onReportQueued() override {}
};

TEST(RdmTestStack, StoresAndSurvivesReboot) {
  MemFileIO io;
  NullInboxHost host;
  RdmTestStack stack(io);
  stack.boot(host);
  ASSERT_FALSE(HasFatalFailure());
  EXPECT_TRUE(stack.recreated);
  EXPECT_EQ(stack.inbox->freeSlots(), stack.in_slots);
  TestKey alice(1, 7, 1);
  RecvInput in = stack.makeRecv(alice.pub, 1790640000u, "hello", true, nullptr, 2, 2, -12);
  EXPECT_EQ(in.flags, 2);
  EXPECT_EQ(in.path_len, 2);
  EXPECT_EQ(in.snr_x4, -12);
  EXPECT_FALSE(in.via_mailbox);
  EXPECT_EQ(stack.inbox->onMessage(in, 5000).result, RecvResult::NEW);

  stack.boot(host);
  ASSERT_FALSE(HasFatalFailure());
  EXPECT_FALSE(stack.recreated);
  EXPECT_EQ(stack.inbox->freeSlots(), stack.in_slots - 1);
  EXPECT_EQ(io.size(Inbox::PATH), (int32_t)RecordFile::fileSize(Inbox::RECORD_SIZE, stack.in_slots, false));

  uint8_t hash[8] = {9, 9, 9, 9, 9, 9, 9, 9};
  RecvInput copy = stack.makeRecv(alice.pub, 1790640001u, "via mailbox", true, hash);
  EXPECT_TRUE(copy.via_mailbox);
  EXPECT_EQ(copy.mbx_hash, hash);
  EXPECT_STREQ(in.text, "hello") << "earlier texts stay valid";
}

// ---- CompanionFrames.h ----

TEST(CompanionFrames, SendTxtLayout) {
  const uint8_t prefix[6] = {1, 2, 3, 4, 5, 6};
  std::vector<uint8_t> f = sim::frames::buildSendTxt(prefix, 0x01020304, 3, "hi");
  const std::vector<uint8_t> want = {2, 0, 3, 0x04, 0x03, 0x02, 0x01, 1, 2, 3, 4, 5, 6, 'h', 'i'};
  EXPECT_EQ(f, want);
}

TEST(CompanionFrames, DecodeSent) {
  sim::frames::SentReply r;
  std::vector<uint8_t> f = {sim::frames::RESP_CODE_SENT, 1, 0x44, 0x33, 0x22, 0x11, 0x10, 0x27, 0, 0};
  ASSERT_TRUE(sim::frames::decodeSent(f, r));
  EXPECT_TRUE(r.flood);
  EXPECT_EQ(r.expected_ack, 0x11223344u);
  EXPECT_EQ(r.est_timeout_ms, 10000u);
  f.pop_back();
  EXPECT_FALSE(sim::frames::decodeSent(f, r)) << "short";
  f = {sim::frames::RESP_CODE_ERR, 1, 0, 0, 0, 0, 0, 0, 0, 0};
  EXPECT_FALSE(sim::frames::decodeSent(f, r));
}

TEST(CompanionFrames, DecodeContactMsg) {
  using namespace sim::frames;
  const std::vector<uint8_t> head = {1, 2, 3, 4, 5, 6, 0xFF, TXT_TYPE_PLAIN, 0x04, 0x03, 0x02, 0x01};
  ContactMsg m;

  std::vector<uint8_t> v1 = {RESP_CODE_CONTACT_MSG_RECV};
  v1.insert(v1.end(), head.begin(), head.end());
  v1.insert(v1.end(), {'o', 'k'});
  ASSERT_TRUE(decodeContactMsg(v1, m));
  EXPECT_EQ(m.sender_prefix[5], 6);
  EXPECT_EQ(m.path_len, 0xFF);
  EXPECT_EQ(m.txt_type, TXT_TYPE_PLAIN);
  EXPECT_EQ(m.ts, 0x01020304u);
  EXPECT_EQ(m.text, "ok");

  std::vector<uint8_t> v3 = {RESP_CODE_CONTACT_MSG_RECV_V3, 0xF0, 0, 0};
  v3.insert(v3.end(), head.begin(), head.end());
  v3.insert(v3.end(), {'v', '3'});
  ASSERT_TRUE(decodeContactMsg(v3, m));
  EXPECT_EQ(m.text, "v3");

  std::vector<uint8_t> signed_msg = v1;
  signed_msg[1 + 7] = TXT_TYPE_SIGNED_PLAIN;
  signed_msg.insert(signed_msg.begin() + 13, {9, 9, 9, 9});
  ASSERT_TRUE(decodeContactMsg(signed_msg, m));
  EXPECT_EQ(m.text, "ok") << "signature skipped";

  EXPECT_FALSE(decodeContactMsg(std::vector<uint8_t>(v1.begin(), v1.begin() + 12), m)) << "short";
  EXPECT_FALSE(decodeContactMsg({RESP_CODE_NO_MORE_MESSAGES}, m));
  EXPECT_FALSE(decodeContactMsg({}, m));
}

TEST(CompanionFrames, ForkCodesComeFromTheFirmwareHeader) {
  EXPECT_EQ(&sim::frames::RESP_CODE_CONTACT_MSG_RECV_V3, &rdm::companion::RESP_CODE_CONTACT_MSG_RECV_V3);
  EXPECT_EQ(sim::frames::CMD_RDM_LIST_OUTBOX, 0xC2);
  EXPECT_EQ(sim::frames::RESP_CODE_RDM_OUTBOX_END, 0x42);
}

}  // namespace

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
