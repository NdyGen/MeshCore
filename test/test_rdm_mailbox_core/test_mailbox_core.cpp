#include <gtest/gtest.h>

#include <helpers/rdm/mailbox/MailboxCore.h>
#include <helpers/rdm/RdmCodec.h>
#include <helpers/rdm/RdmConfig.h>
#include <helpers/rdm/RdmCrypto.h>

#include <MemMailboxBackend.h>

#include <string.h>
#include <vector>

using namespace rdm;

namespace {

const uint32_t T0 = 1790000000;
const uint32_t DAY = 86400;

struct Sent {
  std::vector<uint8_t> client;
  uint32_t tag;
  std::vector<uint8_t> body;
  bool path_return;
};

class FakeHost : public MailboxCoreHost {
public:
  uint32_t now_ms = 1000;
  bool idle = true;
  std::vector<Sent> sent;
  std::vector<std::vector<uint8_t>> peers;

  uint32_t millis() override { return now_ms; }
  bool txIdle() override { return idle; }
  bool addPeer(const uint8_t pub[32]) override {
    peers.emplace_back(pub, pub + 32);
    return true;
  }
  bool sendResponse(const uint8_t client_pub[32], uint32_t tag, const uint8_t* body, size_t len,
                    bool path_return) override {
    sent.push_back({std::vector<uint8_t>(client_pub, client_pub + 32), tag, std::vector<uint8_t>(body, body + len),
                    path_return});
    return true;
  }
  bool hasPeer(const uint8_t pub[32]) const {
    for (auto& p : peers) if (memcmp(p.data(), pub, 32) == 0) return true;
    return false;
  }
};

struct Key {
  uint8_t pub[32];
  explicit Key(uint8_t seed) { for (int i = 0; i < 32; i++) pub[i] = (uint8_t)(seed + i * 7); }
};

class MailboxCoreTest : public ::testing::Test {
protected:
  MemMailboxBackend be;
  FakeHost host;
  MailboxCore core{be, host};
  Key alice{0xA0}, bob{0x10}, carol{0x30};
  uint8_t k_owner[16];
  uint32_t ts = 5000;

  void SetUp() override {
    for (int i = 0; i < 16; i++) k_owner[i] = (uint8_t)(0x55 ^ i);
    be.setTime(T0);
    ASSERT_TRUE(be.addOwner(alice.pub, k_owner, 7, 30, 20));
    core.begin();
  }

  // Every request needs a fresh, rising timestamp and 5 s spacing per client.
  void later(uint32_t ms = 5000) {
    host.now_ms += ms;
    be.setTime(T0 + host.now_ms / 1000);
  }

  // Responses go out one per loop, so loop until everything queued is sent.
  void run() {
    for (int i = 0; i < 2 + 4; i++) core.loop();
  }

  void reg(const Key& who, const Key& owner, bool good_token = true, bool flood = false) {
    uint8_t token[8];
    crypto::tokenB(token, k_owner, who.pub);
    if (!good_token) token[0] ^= 1;
    uint8_t plain[32] = {};
    size_t n = codec::buildRegReq(plain, ++ts, owner.pub, token);
    core.onAnonRequest(who.pub, plain, n + 5, flood);   // decrypted data carries zero padding
  }

  void request(const Key& who, const uint8_t* body, size_t len, bool flood = false, uint32_t req_ts = 0) {
    uint8_t data[200] = {};
    uint32_t t = req_ts ? req_ts : ++ts;
    memcpy(data, &t, 4);
    memcpy(data + 4, body, len);
    core.onRequest(who.pub, data, 4 + len + 3, flood);
  }

  std::vector<uint8_t> inner(uint8_t seed, size_t n = 60) {
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; i++) v[i] = (uint8_t)(seed * 31 + i);
    return v;
  }

  void deposit(const Key& who, const std::vector<uint8_t>& in, const Key& owner, bool flood = false) {
    uint8_t body[200];
    size_t n = codec::buildDeposit(body, owner.pub, in.data(), (uint8_t)in.size());
    ASSERT_GT(n, 0u);
    request(who, body, n, flood);
  }

  void fetch(bool flood, uint8_t flags = 0, const Report* r = nullptr, uint8_t n = 0, uint32_t store_id = 1) {
    uint8_t body[200];
    size_t len = codec::buildFetch(body, flags, store_id, r, n);
    request(alice, body, len, flood);
  }

  Sent last() {
    EXPECT_FALSE(host.sent.empty());
    return host.sent.empty() ? Sent{} : host.sent.back();
  }

  void registered(const Key& who) {
    reg(who, alice);
    run();
    ASSERT_FALSE(host.sent.empty());
    ASSERT_EQ(host.sent.back().body[0], (uint8_t)MbxCode::OK);
    host.sent.clear();
    later();
  }

  struct DepositReply { MbxCode st; uint8_t hash[8]; uint32_t ttl_s; };
  DepositReply depositReply(const Sent& s) {
    DepositReply d{};
    EXPECT_TRUE(codec::parseDepositResp(s.body.data(), s.body.size(), d.st, d.hash, d.ttl_s));
    return d;
  }
};

}

// ---- registration --------------------------------------------------------------------------------------

TEST_F(MailboxCoreTest, RegistrationOkAnswersWithOwnerLimitsAndPiTimeAndCachesThePeer) {
  reg(bob, alice);
  EXPECT_TRUE(host.sent.empty()) << "answer only after the backend";
  run();
  ASSERT_EQ(host.sent.size(), 1u);
  Sent s = host.sent[0];
  EXPECT_EQ(s.tag, ts);
  EXPECT_EQ(0, memcmp(s.client.data(), bob.pub, 32));
  MbxCode st; uint8_t ttl, quota; uint32_t time_m;
  ASSERT_TRUE(codec::parseRegResp(s.body.data(), s.body.size(), st, ttl, quota, time_m));
  EXPECT_EQ(st, MbxCode::OK);
  EXPECT_EQ(ttl, 7);
  EXPECT_EQ(quota, 20);
  EXPECT_EQ(time_m, be.backendTime()) << "tijd_M is the Pi clock";
  EXPECT_TRUE(host.hasPeer(bob.pub));
}

TEST_F(MailboxCoreTest, FloodRegistrationGetsPathReturnDirectOneDoesNot) {
  reg(bob, alice, true, true);
  run();
  ASSERT_EQ(host.sent.size(), 1u);
  EXPECT_TRUE(host.sent[0].path_return);
  later();
  reg(bob, alice, true, false);
  run();
  ASSERT_EQ(host.sent.size(), 2u);
  EXPECT_FALSE(host.sent[1].path_return);
}

TEST_F(MailboxCoreTest, FloodRegistrationFailureAlsoGetsPathReturn) {
  be.setReady(false);
  reg(bob, alice, true, true);
  run();
  ASSERT_EQ(host.sent.size(), 1u);
  EXPECT_EQ(host.sent[0].body[0], (uint8_t)MbxCode::NO_STORAGE);
  EXPECT_TRUE(host.sent[0].path_return);
}

TEST_F(MailboxCoreTest, RegistrationWithBadTokenIsNotAuthAndNotCached) {
  reg(bob, alice, false);
  run();
  ASSERT_EQ(host.sent.size(), 1u);
  EXPECT_EQ(host.sent[0].body[0], (uint8_t)MbxCode::NOT_AUTH);
  EXPECT_FALSE(host.hasPeer(bob.pub));
}

TEST_F(MailboxCoreTest, RegistrationForUnknownOwner) {
  reg(bob, carol);
  run();
  ASSERT_EQ(host.sent.size(), 1u);
  EXPECT_EQ(host.sent[0].body[0], (uint8_t)MbxCode::UNKNOWN_OWNER);
}

TEST_F(MailboxCoreTest, MalformedAnonRequestIsIgnored) {
  uint8_t plain[19] = {1, 0, 0, 0, 0xFF, 'X', 1};
  core.onAnonRequest(bob.pub, plain, sizeof(plain), false);
  core.onAnonRequest(bob.pub, plain, 5, false);
  run();
  EXPECT_TRUE(host.sent.empty());
  EXPECT_EQ(be.requestCount(), 0u);
}

TEST_F(MailboxCoreTest, AnonRequestsAreLimitedGloballyToFourPerMinute) {
  Key k[5] = {Key(1), Key(2), Key(3), Key(4), Key(5)};
  for (auto& who : k) reg(who, alice);
  run();
  EXPECT_EQ(host.sent.size(), 4u) << "the fifth is dropped without an answer";
  EXPECT_EQ(be.requestCount(), 4u);
  host.now_ms += 60000;
  reg(k[4], alice);
  run();
  EXPECT_EQ(host.sent.size(), 5u);
}

// ---- deposit -------------------------------------------------------------------------------------------

TEST_F(MailboxCoreTest, DepositStoredAnswersTtlAndPacketHash) {
  registered(bob);
  auto in = inner(1);
  deposit(bob, in, alice);
  run();
  ASSERT_EQ(host.sent.size(), 1u);
  EXPECT_EQ(host.sent[0].tag, ts);
  EXPECT_FALSE(host.sent[0].path_return);
  DepositReply d = depositReply(host.sent[0]);
  EXPECT_EQ(d.st, MbxCode::OK);
  EXPECT_EQ(d.ttl_s, 7 * DAY) << "ttl_s = expires - backendTime";
  uint8_t h[8];
  crypto::txtPacketHash(h, in.data(), in.size());
  EXPECT_EQ(0, memcmp(d.hash, h, 8));
  MemMailboxBackend::MsgState st;
  EXPECT_TRUE(be.messageState(h, st));
}

TEST_F(MailboxCoreTest, DepositAlreadyStoredCountsDownTtl) {
  registered(bob);
  auto in = inner(1);
  deposit(bob, in, alice);
  run();
  later(3600 * 1000);
  deposit(bob, in, alice);
  run();
  ASSERT_EQ(host.sent.size(), 2u);
  DepositReply d = depositReply(host.sent[1]);
  EXPECT_EQ(d.st, MbxCode::ALREADY_STORED);
  EXPECT_EQ(d.ttl_s, 7 * DAY - 3600);
}

TEST_F(MailboxCoreTest, DepositFromUnregisteredSenderIsNotAuth) {
  auto in = inner(1);
  deposit(bob, in, alice);
  run();
  ASSERT_EQ(host.sent.size(), 1u);
  DepositReply d = depositReply(host.sent[0]);
  EXPECT_EQ(d.st, MbxCode::NOT_AUTH);
  EXPECT_EQ(d.ttl_s, 0u);
}

TEST_F(MailboxCoreTest, DepositForUnknownOwner) {
  registered(bob);
  deposit(bob, inner(1), carol);
  run();
  EXPECT_EQ(depositReply(last()).st, MbxCode::UNKNOWN_OWNER);
}

TEST_F(MailboxCoreTest, DepositOverQuota) {
  ASSERT_TRUE(be.addOwner(alice.pub, k_owner, 7, 30, 1));
  registered(bob);
  deposit(bob, inner(1), alice);
  run();
  later();
  deposit(bob, inner(2), alice);
  run();
  EXPECT_EQ(depositReply(last()).st, MbxCode::QUOTA);
}

TEST_F(MailboxCoreTest, DepositTooBigNeverReachesTheBackend) {
  registered(bob);
  uint32_t before = be.requestCount();
  uint8_t body[200] = {REQ_DEPOSIT};
  memcpy(body + 1, alice.pub, 4);
  body[5] = MAX_INNER_PAYLOAD + 1;
  for (size_t i = 0; i < MAX_INNER_PAYLOAD + 1; i++) body[6 + i] = (uint8_t)(i + 1);
  request(bob, body, 6 + MAX_INNER_PAYLOAD + 1);
  EXPECT_EQ(be.requestCount(), before);
  run();
  ASSERT_EQ(host.sent.size(), 1u);
  DepositReply d = depositReply(host.sent[0]);
  EXPECT_EQ(d.st, MbxCode::TOO_BIG);
  uint8_t h[8];
  crypto::txtPacketHash(h, body + 6, MAX_INNER_PAYLOAD + 1);
  EXPECT_EQ(0, memcmp(d.hash, h, 8));
  EXPECT_EQ(be.requestCount(), before);
}

TEST_F(MailboxCoreTest, NoBackendAnswerWithinThreeSecondsIsNoStorage) {
  registered(bob);
  be.setMode(MemMailboxBackend::Mode::NO_REPLY);
  deposit(bob, inner(1), alice);
  run();
  EXPECT_EQ(core.nextWakeupMillis(host.now_ms), 3000u);
  host.now_ms += 2999;
  run();
  EXPECT_TRUE(host.sent.empty());
  EXPECT_EQ(core.nextWakeupMillis(host.now_ms), 1u);
  host.now_ms += 1;
  EXPECT_EQ(core.nextWakeupMillis(host.now_ms), 0u);
  run();
  ASSERT_EQ(host.sent.size(), 1u);
  EXPECT_EQ(depositReply(host.sent[0]).st, MbxCode::NO_STORAGE);
  EXPECT_EQ(core.nextWakeupMillis(host.now_ms), UINT32_MAX);
}

TEST_F(MailboxCoreTest, RegistrationWithoutBackendAnswerIsNoStorage) {
  be.setMode(MemMailboxBackend::Mode::NO_REPLY);
  reg(bob, alice);
  host.now_ms += 3000;
  run();
  ASSERT_EQ(host.sent.size(), 1u);
  EXPECT_EQ(host.sent[0].body[0], (uint8_t)MbxCode::NO_STORAGE);
  EXPECT_FALSE(host.hasPeer(bob.pub));
}

TEST_F(MailboxCoreTest, LateBackendReplyAfterTimeoutIsIgnored) {
  registered(bob);
  be.setMode(MemMailboxBackend::Mode::NO_REPLY);
  deposit(bob, inner(1), alice);
  host.now_ms += 3000;
  run();
  be.setMode(MemMailboxBackend::Mode::NORMAL);
  ASSERT_EQ(host.sent.size(), 1u);
  // A reply for the old id may still arrive from a slow Pi: nobody waits for it any more.
  uint8_t h[8] = {}, p[1] = {0};
  be.store(1, alice.pub, bob.pub, h, p, 1);
  run();
  EXPECT_EQ(host.sent.size(), 1u);
}

TEST_F(MailboxCoreTest, FetchAndStatusWithoutBackendAnswerGetNothing) {
  registered(bob);
  be.setMode(MemMailboxBackend::Mode::NO_REPLY);
  fetch(false);
  later();
  uint8_t hashes[1][8] = {{1}};
  uint8_t body[20];
  request(bob, body, codec::buildStatus(body, hashes, 1));
  host.now_ms += 3000;
  run();
  EXPECT_TRUE(host.sent.empty());
  EXPECT_EQ(core.nextWakeupMillis(host.now_ms), UINT32_MAX);
}

TEST_F(MailboxCoreTest, BackendNotReadyAnswersNoStorageAtOnce) {
  be.setReady(false);
  reg(bob, alice);
  later();
  deposit(carol, inner(1), alice);
  later();
  fetch(false);
  EXPECT_EQ(be.requestCount(), 0u);
  run();
  ASSERT_EQ(host.sent.size(), 2u) << "FETCH gets nothing";
  EXPECT_EQ(host.sent[0].body[0], (uint8_t)MbxCode::NO_STORAGE);
  EXPECT_EQ(depositReply(host.sent[1]).st, MbxCode::NO_STORAGE);
}

TEST_F(MailboxCoreTest, UnknownPiClockFallsBackToOwnerTtlFromRegistration) {
  be.setTime(0);
  ASSERT_TRUE(be.addOwner(alice.pub, k_owner, 3, 30, 20));
  registered(bob);
  be.setTime(0);
  deposit(bob, inner(1), alice);
  run();
  EXPECT_EQ(depositReply(last()).ttl_s, 3 * DAY);
}

TEST_F(MailboxCoreTest, UnknownPiClockAndUnknownOwnerTtlUsesDefaultTRadio) {
  uint8_t token[8];   // registered at the Pi before this radio booted: the core never saw the REG reply
  crypto::tokenB(token, k_owner, bob.pub);
  ASSERT_TRUE(be.reg(99, bob.pub, alice.pub, token));
  BackendReply r;
  while (be.poll(r)) {}
  be.setTime(0);
  deposit(bob, inner(1), alice);
  run();
  EXPECT_EQ(depositReply(last()).ttl_s, (uint32_t)RDM_T_RADIO_S);
}

TEST_F(MailboxCoreTest, PendingTableFullIsNoStorage) {
  be.setMode(MemMailboxBackend::Mode::NO_REPLY);
  for (int i = 0; i < 9; i++) {
    Key k((uint8_t)(0x40 + i));
    deposit(k, inner(1), alice);
  }
  run();
  ASSERT_EQ(host.sent.size(), 1u) << "the ninth request finds no free slot";
  EXPECT_EQ(depositReply(host.sent[0]).st, MbxCode::NO_STORAGE);
  EXPECT_EQ(be.requestCount(), 8u);
}

// ---- replay and rate limits ----------------------------------------------------------------------------

TEST_F(MailboxCoreTest, OlderTimestampIsDroppedEqualIsAccepted) {
  registered(bob);
  uint8_t hashes[1][8] = {{1}};
  uint8_t body[20];
  size_t n = codec::buildStatus(body, hashes, 1);
  request(bob, body, n, false, 9000);
  run();
  ASSERT_EQ(host.sent.size(), 1u);
  later();
  request(bob, body, n, false, 8999);
  run();
  EXPECT_EQ(host.sent.size(), 1u) << "replayed timestamp";
  later();
  request(bob, body, n, false, 9000);
  run();
  EXPECT_EQ(host.sent.size(), 2u);
}

TEST_F(MailboxCoreTest, OneRequestPerFiveSecondsPerClient) {
  registered(bob);
  deposit(bob, inner(1), alice);
  run();
  host.now_ms += 4999;
  deposit(bob, inner(2), alice);
  run();
  ASSERT_EQ(host.sent.size(), 2u);
  EXPECT_EQ(depositReply(host.sent[1]).st, MbxCode::RATE_LIMITED);
  EXPECT_EQ(depositReply(host.sent[1]).ttl_s, 0u);
  fetch(false);   // alice is another client: not limited by bob
  run();
  EXPECT_EQ(host.sent.size(), 3u);
  host.now_ms += 1;
  deposit(bob, inner(2), alice);
  run();
  EXPECT_EQ(depositReply(last()).st, MbxCode::OK);
}

TEST_F(MailboxCoreTest, RateLimitedFetchAndStatusAreDropped) {
  fetch(false);
  run();
  ASSERT_EQ(host.sent.size(), 1u);
  host.now_ms += 1000;
  fetch(false);
  run();
  EXPECT_EQ(host.sent.size(), 1u);
}

TEST_F(MailboxCoreTest, SixtyRequestsPerHourPerClient) {
  uint32_t start = host.now_ms;
  for (int i = 0; i < 60; i++) {
    fetch(false);
    run();
    later();
  }
  EXPECT_EQ(host.sent.size(), 60u);
  fetch(false);
  run();
  EXPECT_EQ(host.sent.size(), 60u) << "61st within the hour";
  host.now_ms = start + 3600 * 1000;
  fetch(false);
  run();
  EXPECT_EQ(host.sent.size(), 61u);
}

TEST_F(MailboxCoreTest, UnknownRequestTypeIsIgnoredAndNotCounted) {
  uint8_t body[4] = {REQ_RECEIPT_QUERY, 0};
  request(bob, body, 2);
  run();
  EXPECT_TRUE(host.sent.empty());
  deposit(bob, inner(1), alice);   // no 5 s wait needed: the unknown request did not count
  run();
  EXPECT_EQ(host.sent.size(), 1u);
}

// ---- fetch and status ----------------------------------------------------------------------------------

TEST_F(MailboxCoreTest, FloodFetchGetsPathReturnWithoutCopyThenDirectFetchCarriesIt) {
  registered(bob);
  auto in = inner(7);
  deposit(bob, in, alice);
  run();
  host.sent.clear();

  fetch(true);
  run();
  ASSERT_EQ(host.sent.size(), 1u);
  EXPECT_TRUE(host.sent[0].path_return);
  uint8_t remaining, ok, inner_len;
  const uint8_t* got;
  ASSERT_TRUE(codec::parseFetchResp(host.sent[0].body.data(), host.sent[0].body.size(), remaining, ok, got, inner_len));
  EXPECT_EQ(remaining, 1);
  EXPECT_EQ(inner_len, 0);
  uint8_t h[8];
  crypto::txtPacketHash(h, in.data(), in.size());
  MemMailboxBackend::MsgState st;
  ASSERT_TRUE(be.messageState(h, st));
  EXPECT_EQ(st, MemMailboxBackend::MsgState::STORED) << "backend was asked with NO_PAYLOAD";

  later();
  fetch(false);
  run();
  ASSERT_EQ(host.sent.size(), 2u);
  EXPECT_FALSE(host.sent[1].path_return);
  ASSERT_TRUE(codec::parseFetchResp(host.sent[1].body.data(), host.sent[1].body.size(), remaining, ok, got, inner_len));
  EXPECT_EQ(remaining, 0);
  ASSERT_EQ(inner_len, in.size());
  EXPECT_EQ(0, memcmp(got, in.data(), in.size()));
}

TEST_F(MailboxCoreTest, FetchReportsAreConfirmed) {
  registered(bob);
  auto in = inner(7);
  deposit(bob, in, alice);
  run();
  fetch(false);
  run();
  later();
  Report r;
  crypto::txtPacketHash(r.pkt_hash, in.data(), in.size());
  r.result = ReportResult::ON_RADIO;
  memset(r.ack, 0xAB, 6);
  fetch(false, 0, &r, 1);
  run();
  uint8_t remaining, ok, inner_len;
  const uint8_t* got;
  Sent s = last();
  ASSERT_TRUE(codec::parseFetchResp(s.body.data(), s.body.size(), remaining, ok, got, inner_len));
  EXPECT_EQ(ok, 1);
  MemMailboxBackend::MsgState st;
  ASSERT_TRUE(be.messageState(r.pkt_hash, st));
  EXPECT_EQ(st, MemMailboxBackend::MsgState::ON_RADIO);
}

TEST_F(MailboxCoreTest, FetchFromNonOwnerGetsNothing) {
  uint8_t body[20];
  request(bob, body, codec::buildFetch(body, 0, 1, nullptr, 0));
  run();
  EXPECT_TRUE(host.sent.empty());
}

TEST_F(MailboxCoreTest, StatusAnswersPerHash) {
  registered(bob);
  auto in = inner(3);
  deposit(bob, in, alice);
  run();
  later();
  uint8_t hashes[2][8] = {};
  crypto::txtPacketHash(hashes[0], in.data(), in.size());
  hashes[1][0] = 0x99;
  uint8_t body[40];
  request(bob, body, codec::buildStatus(body, hashes, 2), true);
  run();
  Sent s = last();
  EXPECT_TRUE(s.path_return);
  StatusReply items[MAX_BATCH];
  uint8_t n;
  ASSERT_TRUE(codec::parseStatusResp(s.body.data(), s.body.size(), items, n));
  ASSERT_EQ(n, 2);
  EXPECT_EQ(items[0].state, MbxState::STORED);
  EXPECT_EQ(items[1].state, MbxState::UNKNOWN);
}

// ---- pacing, ACL, H4 -----------------------------------------------------------------------------------

TEST_F(MailboxCoreTest, ResponsesWaitForAnIdleTransmitterOneAtATime) {
  host.idle = false;
  reg(bob, alice);
  reg(carol, alice);
  run();
  EXPECT_TRUE(host.sent.empty());
  EXPECT_EQ(core.nextWakeupMillis(host.now_ms), 0u) << "queued responses are due now";
  host.idle = true;
  core.loop();
  EXPECT_EQ(host.sent.size(), 1u);
  core.loop();
  EXPECT_EQ(host.sent.size(), 2u);
  EXPECT_EQ(core.nextWakeupMillis(host.now_ms), UINT32_MAX);
}

TEST_F(MailboxCoreTest, AclFromBackendFillsThePeerCacheAfterHello) {
  registered(bob);
  host.peers.clear();
  MailboxCore fresh(be, host);   // radio reboot
  fresh.begin();
  fresh.loop();
  EXPECT_TRUE(host.peers.empty()) << "nothing before the Pi answers HELLO";
  be.hello();
  fresh.loop();
  EXPECT_TRUE(host.hasPeer(alice.pub));
  EXPECT_TRUE(host.hasPeer(bob.pub));
}

TEST_F(MailboxCoreTest, NextWakeupIsNeverLate) {
  registered(bob);
  be.setMode(MemMailboxBackend::Mode::NO_REPLY);
  deposit(bob, inner(1), alice);
  host.now_ms += 1234;
  later(0);
  uint32_t w = core.nextWakeupMillis(host.now_ms);
  ASSERT_EQ(w, 3000u - 1234u);
  host.now_ms += w - 1;
  run();
  EXPECT_TRUE(host.sent.empty()) << "loop(nextDue - 1) does nothing";
  host.now_ms += 1;
  run();
  EXPECT_EQ(host.sent.size(), 1u) << "loop(nextDue) answers";
}

TEST_F(MailboxCoreTest, NextWakeupSurvivesMillisWrap) {
  host.now_ms = 0xFFFFFFFFu - 1000;
  be.setMode(MemMailboxBackend::Mode::NO_REPLY);
  deposit(bob, inner(1), alice);
  EXPECT_EQ(core.nextWakeupMillis(host.now_ms), 3000u);
  host.now_ms += 2999;   // wraps
  run();
  EXPECT_TRUE(host.sent.empty());
  host.now_ms += 1;
  run();
  EXPECT_EQ(host.sent.size(), 1u);
}
