// Unit tests of rdm::Outbox against 03-ontwerp.md par. 4 and 11 (G1-G16) with a fake clock (RDM seconds).

#include <gtest/gtest.h>

#include <helpers/rdm/RdmConfig.h>
#include <helpers/rdm/RdmCrypto.h>
#include <helpers/rdm/RdmOutbox.h>

#include <MemFileIO.h>
#include <TestUtil.h>
#include <Utils.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

using namespace rdm;

namespace {

const uint32_t T0 = 1800000000UL;
const uint32_t DAY = 86400;
const uint8_t ALICE[6] = {0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6};
const uint8_t CAROL[6] = {0xC1, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6};
const uint8_t MBX1[32] = {0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37};
const uint8_t MBX2[32] = {0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47};

struct Tx {
  enum Kind { DM, QUERY, DEPOSIT, STATUS, REGISTER } kind;
  uint32_t at;
  std::vector<uint8_t> prefix;
  uint32_t ts;
  uint8_t attempt;
  bool flood;
  std::vector<QueryItem> items;
  std::vector<std::vector<uint8_t>> hashes;
  std::vector<uint8_t> hash;   // DEPOSIT: the pkt_hash handed back
};

struct Pushed {
  OutEntry e;
  UserStatus s;
};

class FakeHost : public OutboxHost {
public:
  uint8_t self[32];
  uint32_t now = T0;
  uint32_t rnd = 0;
  bool contact = true, direct = true, idle = true, send_ok = true;
  bool rdm_client = true, app = true;
  uint32_t est_ms = 5000;
  std::vector<Tx> tx;
  std::vector<Pushed> status;
  std::vector<uint32_t> confirmed;
  uint32_t idle_calls = 0;   // one per loop() that got as far as the transmit check

  FakeHost() { fillPattern(self, sizeof(self), 0xB0, 1); }

  void selfPub(uint8_t pub_out[32]) override { memcpy(pub_out, self, 32); }
  bool lcg = false;   // true: the `seq` sequence instead of the constant `rnd`
  Lcg32 seq;
  uint32_t random32() override { return lcg ? seq.next() >> 8 : rnd; }
  bool contactPub(const uint8_t pub_prefix[6], uint8_t pub_out[32]) override {
    memset(pub_out, 0, 32);
    memcpy(pub_out, pub_prefix, 6);
    return contact;
  }
  bool hasDirectPath(const uint8_t*) override { return direct; }
  bool txIdle() override {
    idle_calls++;
    return idle;
  }
  bool sendDm(const OutEntry& e, uint8_t attempt, bool flood, uint32_t& est) override {
    Tx t{Tx::DM, now, std::vector<uint8_t>(e.pub_prefix, e.pub_prefix + 6), e.ts, attempt, flood, {}, {}, {}};
    tx.push_back(t);
    est = est_ms;
    return send_ok;
  }
  bool sendReceiptQuery(const uint8_t pub_prefix[6], const QueryItem* q, uint8_t n, bool flood, uint32_t& est) override {
    Tx t{Tx::QUERY, now, std::vector<uint8_t>(pub_prefix, pub_prefix + 6), 0, 0, flood, std::vector<QueryItem>(q, q + n), {}, {}};
    tx.push_back(t);
    est = est_ms;
    return send_ok;
  }
  bool sendDeposit(const OutEntry& e, uint8_t attempt, uint8_t hash_out[8], uint32_t& est) override {
    uint8_t h[8] = {0xD0, attempt, 0, 0, 0, 0, e.pub_prefix[0], 1};
    memcpy(h + 2, &e.ts, 4);
    memcpy(hash_out, h, 8);
    Tx t{Tx::DEPOSIT, now, std::vector<uint8_t>(e.pub_prefix, e.pub_prefix + 6), e.ts, attempt, false, {}, {},
         std::vector<uint8_t>(h, h + 8)};
    tx.push_back(t);
    est = est_ms;
    return send_ok;
  }
  bool sendStatus(const uint8_t pub_prefix[6], const uint8_t (*hashes)[8], uint8_t n, uint32_t& est) override {
    Tx t{Tx::STATUS, now, std::vector<uint8_t>(pub_prefix, pub_prefix + 6), 0, 0, false, {}, {}, {}};
    for (uint8_t i = 0; i < n; i++) t.hashes.push_back(std::vector<uint8_t>(hashes[i], hashes[i] + 8));
    tx.push_back(t);
    est = est_ms;
    return send_ok;
  }
  bool sendRegister(const uint8_t pub_prefix[6], uint32_t& est) override {
    Tx t{Tx::REGISTER, now, std::vector<uint8_t>(pub_prefix, pub_prefix + 6), 0, 0, false, {}, {}, {}};
    tx.push_back(t);
    est = est_ms;
    return send_ok;
  }
  bool pushUserStatus(const OutEntry& e, UserStatus s) override {
    if (!rdm_client) return false;
    status.push_back({e, s});
    return true;
  }
  bool pushSendConfirmed(const OutEntry& e) override {
    if (!app) return false;
    confirmed.push_back(e.app_ack);
    return true;
  }

  size_t count(Tx::Kind k) const {
    size_t n = 0;
    for (const Tx& t : tx) n += t.kind == k;
    return n;
  }
  const Tx* last(Tx::Kind k) const {
    for (auto it = tx.rbegin(); it != tx.rend(); ++it) {
      if (it->kind == k) return &*it;
    }
    return nullptr;
  }
};

class OutboxTest : public ::testing::Test {
protected:
  MemFileIO io;
  FakeHost host;
  std::unique_ptr<RecordFile> file, wfile, cfile;
  std::unique_ptr<ContactTable> contacts;
  std::unique_ptr<Outbox> ob;
  uint32_t now = T0;

  void SetUp() override { boot(T0); }

  // power cycle: all RAM objects new, files stay
  void boot(uint32_t at) {
    ob.reset();
    contacts.reset();
    file.reset(new RecordFile(io, Outbox::PATH, 1, Outbox::RECORD_SIZE, 8, true));
    wfile.reset(new RecordFile(io, Outbox::WATCH_PATH, 1, Outbox::WATCH_RECORD_SIZE, 32, false));
    cfile.reset(new RecordFile(io, ContactTable::PATH, 1, ContactTable::RECORD_SIZE, 16, false));
    ASSERT_NE(cfile->open(), RecordFile::Open::FAILED);
    contacts.reset(new ContactTable(*cfile));
    ASSERT_TRUE(contacts->begin());
    ob.reset(new Outbox(*file, *wfile, *contacts, host));
    now = host.now = at;
    ASSERT_TRUE(ob->begin(at));
  }

  void loopAt(uint32_t t) {
    now = host.now = t;
    ob->loop(t);
  }

  // H4 at every step: loop(nextDue - 1) does nothing observable; stops before `to` or `until` holds
  template <typename Pred> uint32_t runUntil(uint32_t to, Pred until) {
    for (int guard = 0; guard < 20000; guard++) {
      if (until()) return now;
      uint32_t d = ob->nextDue(now);
      if (d > to) break;
      if (d > now + 1) expectIdle(d - 1);
      loopAt(d);
      if (ob->nextDue(d) == d && !until()) {
        ADD_FAILURE() << "loop(" << d << ") left work due at the same second";
        break;
      }
    }
    if (now < to && !until()) now = host.now = to;
    return now;
  }
  void runTo(uint32_t to) {
    runUntil(to, [] { return false; });
  }
  uint32_t runToTx(uint32_t limit, Tx::Kind k) {
    size_t n = host.count(k);
    return runUntil(limit, [&] { return host.count(k) > n; });
  }

  void expectIdle(uint32_t t) {
    size_t tx = host.tx.size(), st = host.status.size(), cf = host.confirmed.size();
    std::vector<OutState> states = allStates();
    loopAt(t);
    EXPECT_EQ(host.tx.size(), tx) << "sent at " << t - T0;
    EXPECT_EQ(host.status.size(), st) << "0x91 at " << t - T0;
    EXPECT_EQ(host.confirmed.size(), cf);
    EXPECT_EQ(allStates(), states) << "state change at " << t - T0;
  }

  std::vector<OutState> allStates() {
    std::vector<OutState> v;
    OutEntry e;
    for (uint8_t i = 0; ob->get(i, e); i++) v.push_back(e.state);
    return v;
  }

  OutEntry entry(uint8_t idx = 0) {
    OutEntry e;
    memset(&e, 0, sizeof(e));
    EXPECT_TRUE(ob->get(idx, e));
    return e;
  }
  OutState state(uint8_t idx = 0) { return entry(idx).state; }

  uint32_t send(const char* text, uint32_t ts = 1000, uint8_t attempt = 0, const uint8_t* to = ALICE) {
    uint32_t ack = 0;
    bool transmit = false;
    Outbox::SendResult r = ob->onAppSend(to, ts, attempt, text, strlen(text), now, ack, transmit);
    EXPECT_TRUE(r == Outbox::SendResult::NEW_ENTRY || r == Outbox::SendResult::EXISTING_ENTRY);
    if (transmit) ob->onAppTransmitted(to, ts, host.est_ms, now);
    return ack;
  }

  void ackR(uint8_t out[4], const char* text, uint32_t ts = 1000, uint8_t cls = 0) {
    crypto::ackR(out, ts, cls, text, strlen(text), host.self);
  }
  void ackS(uint8_t out[6], const char* text, uint32_t ts = 1000) {
    crypto::ackS(out, ts, 0, text, strlen(text), host.self);
  }
  QueryItem item(const char* text, uint32_t ts = 1000) {
    QueryItem q;
    q.ts = ts;
    crypto::key(q.key, ts, text, strlen(text), host.self);
    return q;
  }
  bool sendAck(const char* text, bool cap, uint32_t ts = 1000, uint8_t cls = 0) {
    uint8_t a[7];
    ackR(a, text, ts, cls);
    a[4] = 252;
    a[5] = 0x5A;
    a[6] = CAP_BYTE;
    return ob->onAck(a, cap ? 7 : 6, now);
  }
  bool sendAckS(const char* text, uint32_t ts = 1000) {
    uint8_t a[6];
    ackS(a, text, ts);
    return ob->onAck(a, 6, now);
  }
  void reply(const char* text, QueryState st, const uint8_t* ack = nullptr, uint32_t ts = 1000, const uint8_t* to = ALICE) {
    QueryItem q = item(text, ts);
    QueryReply r;
    r.state = st;
    memset(r.ack, 0, 6);
    if (ack) memcpy(r.ack, ack, st == QueryState::SYNCED ? 6 : 4);
    ob->onQueryReply(to, &q, &r, 1, now);
  }
  void statusReply(MbxState st, const uint8_t* ack = nullptr, size_t ack_len = 0, uint8_t idx = 0) {
    OutEntry e = entry(idx);
    uint8_t h[1][8];
    memcpy(h[0], e.pkt_hash, 8);
    StatusReply r;
    r.state = st;
    memset(r.ack, 0, 6);
    if (ack) memcpy(r.ack, ack, ack_len);
    ob->onStatusReply(e.pub_prefix, h, &r, 1, now);
  }

  ContactRdm* contact(const uint8_t* pfx = ALICE) { return contacts->findOrAdd(pfx); }
  void setCap(const uint8_t* pfx = ALICE) {
    ContactRdm* c = contact(pfx);
    c->flags |= CR_CAP;
    contacts->save(*c);
  }
  bool capOf(const uint8_t* pfx = ALICE) {
    ContactRdm* c = contacts->find(pfx);
    return c && (c->flags & CR_CAP);
  }
  void setMailbox(const uint8_t* mbx, const uint8_t* pfx = ALICE) {
    ContactRdm* c = contact(pfx);
    if (mbx) {
      c->flags |= CR_HAS_MBX;
      memcpy(c->mbx_pub, mbx, 32);
    } else {
      c->flags &= ~CR_HAS_MBX;
    }
    contacts->save(*c);
  }

  // runs `count` ON_RADIO queries, answering each so no G17b repeat happens; returns their times - base
  std::vector<uint32_t> answeredQueries(const char* text, int count, uint32_t base) {
    std::vector<uint32_t> v;
    uint8_t a[4];
    ackR(a, text);
    for (int k = 0; k < count; k++) {
      size_t n = host.count(Tx::QUERY);
      runToTx(now + 3 * DAY, Tx::QUERY);
      if (host.count(Tx::QUERY) == n) break;
      v.push_back(now - base);
      now += 1;
      reply(text, QueryState::ON_RADIO, a);
    }
    return v;
  }
  std::vector<uint32_t> answeredStatuses(int count, uint32_t base) {
    std::vector<uint32_t> v;
    for (int k = 0; k < count; k++) {
      size_t n = host.count(Tx::STATUS);
      runToTx(now + 3 * DAY, Tx::STATUS);
      if (host.count(Tx::STATUS) == n) break;
      v.push_back(now - base);
      now += 1;
      statusReply(MbxState::STORED);
    }
    return v;
  }

  UserStatus lastStatus() { return host.status.empty() ? (UserStatus)0xFF : host.status.back().s; }

  // WAIT_ACK timeout of a message sent at `now` with est 5 s
  void timeout() { loopAt(now + RDM_WAIT_ACK_MIN_S); }

  // message in CUSTODY at M; returns the time it entered CUSTODY
  uint32_t toCustody(const char* text, uint32_t ttl_s = 10 * DAY) {
    setMailbox(MBX1);
    send(text);
    uint8_t idx = ob->count() - 1;
    timeout();
    EXPECT_EQ(state(idx), OutState::DEPOSITING);
    const Tx* d = host.last(Tx::DEPOSIT);
    EXPECT_NE(d, nullptr);
    ob->onDepositReply(MbxCode::OK, d->hash.data(), ttl_s, now);
    EXPECT_EQ(state(idx), OutState::CUSTODY);
    return now;
  }
};

std::vector<uint32_t> times(const std::vector<Tx>& tx, Tx::Kind k, uint32_t base) {
  std::vector<uint32_t> v;
  for (const Tx& t : tx) {
    if (t.kind == k) v.push_back(t.at - base);
  }
  return v;
}

}

// ---- app side, entries, ACK_R (03 par. 1a, 1d) ----

TEST_F(OutboxTest, NewEntryAppAckEqualsUpstreamComposeMsgPacket) {
  uint32_t ack = 0;
  bool transmit = false;
  EXPECT_EQ(ob->onAppSend(ALICE, 1234, 1, "hello", 5, now, ack, transmit), Outbox::SendResult::NEW_ENTRY);
  EXPECT_TRUE(transmit);

  // BaseChatMesh::composeMsgPacket: sha256(ts | attempt & 3 | text, self_id.pub_key)[0:4]
  uint8_t tmp[10];
  uint32_t ts = 1234;
  memcpy(tmp, &ts, 4);
  tmp[4] = 1;
  memcpy(tmp + 5, "hello", 5);
  uint32_t expected = 0;
  mesh::Utils::sha256((uint8_t*)&expected, 4, tmp, 10, host.self, 32);
  EXPECT_EQ(ack, expected);

  OutEntry e = entry();
  EXPECT_EQ(e.state, OutState::NEW);
  EXPECT_EQ(e.klass, 1);
  EXPECT_EQ(e.fw_attempt, FW_ATTEMPT_BASE + 1);
  EXPECT_EQ(e.app_ack, ack);
  EXPECT_EQ(e.created, T0);
  EXPECT_EQ(e.deadline, T0 + RDM_T_RADIO_S);
  EXPECT_EQ(e.final_at, 0u);
  EXPECT_EQ(std::string(e.text, e.text_len), "hello");
  uint8_t k[4];
  crypto::key(k, 1234, "hello", 5, host.self);
  EXPECT_EQ(0, memcmp(e.key, k, 4));
  EXPECT_TRUE(host.tx.empty()) << "the app transmits the first attempt itself";
}

TEST_F(OutboxTest, AppRetryLandsOnSameEntryWithNewAppAck) {
  uint32_t a0 = send("hi", 1000, 0);
  now += 10;
  uint32_t a1 = 0;
  bool transmit = false;
  EXPECT_EQ(ob->onAppSend(ALICE, 1000, 1, "hi", 2, now, a1, transmit), Outbox::SendResult::EXISTING_ENTRY);
  EXPECT_TRUE(transmit);
  EXPECT_NE(a0, a1);
  EXPECT_EQ(ob->count(), 1);
  EXPECT_EQ(entry().app_ack, a1);
  EXPECT_EQ(entry().klass, 0) << "class stays that of the first attempt";

  uint32_t other = 0;
  EXPECT_EQ(ob->onAppSend(ALICE, 1000, 0, "ho", 2, now, other, transmit), Outbox::SendResult::NEW_ENTRY)
      << "same timestamp, other text: other K";
  EXPECT_EQ(ob->count(), 2);
}

TEST_F(OutboxTest, AppRetryIsNotTransmittedOnceOnRadioOrInCustody) {
  send("hi");
  ASSERT_TRUE(sendAck("hi", true));
  uint32_t a = 0;
  bool transmit = true;
  EXPECT_EQ(ob->onAppSend(ALICE, 1000, 1, "hi", 2, now, a, transmit), Outbox::SendResult::EXISTING_ENTRY);
  EXPECT_FALSE(transmit);

  toCustody("custody text");
  transmit = true;
  ob->onAppSend(ALICE, 1000, 1, "custody text", 12, now, a, transmit);
  EXPECT_FALSE(transmit);
}

TEST_F(OutboxTest, TextOverLimitIsTooLongWithoutEntry) {
  std::string t(MAX_TEXT + 1, 'x');
  uint32_t ack = 0;
  bool transmit = false;
  EXPECT_EQ(ob->onAppSend(ALICE, 1, 0, t.c_str(), t.size(), now, ack, transmit), Outbox::SendResult::TOO_LONG);
  EXPECT_TRUE(transmit);
  EXPECT_NE(ack, 0u);
  EXPECT_EQ(ob->count(), 0);
  t.resize(MAX_TEXT);
  EXPECT_EQ(ob->onAppSend(ALICE, 1, 0, t.c_str(), t.size(), now, ack, transmit), Outbox::SendResult::NEW_ENTRY);
}

TEST_F(OutboxTest, EntryThatCannotBePersistedIsNoOutbox) {
  io.failWritesAfter(0);
  uint32_t ack = 0;
  bool transmit = false;
  EXPECT_EQ(ob->onAppSend(ALICE, 1, 0, "x", 1, now, ack, transmit), Outbox::SendResult::NO_OUTBOX);
  EXPECT_TRUE(transmit) << "falls back to a plain DM";
  EXPECT_EQ(ob->count(), 0);
}

// ---- G1, G3 ----

TEST_F(OutboxTest, G1_FirstQueuedRightAfterTransmitAndOnlyOnChange) {
  uint32_t ack = 0;
  bool transmit = false;
  ob->onAppSend(ALICE, 1000, 0, "hi", 2, now, ack, transmit);
  EXPECT_TRUE(host.status.empty()) << "not before RESP_CODE_SENT";
  ob->onAppTransmitted(ALICE, 1000, 5000, now);
  ASSERT_EQ(host.status.size(), 1u);
  EXPECT_EQ(host.status[0].s, UserStatus::QUEUED);
  EXPECT_EQ(host.status[0].e.app_ack, ack);
  timeout();
  EXPECT_EQ(state(), OutState::RETRY);
  EXPECT_EQ(host.status.size(), 1u) << "RETRY is still QUEUED";
}

TEST_F(OutboxTest, G1_QueuedFromLoopWhenAppNeverReportsTransmit) {
  uint32_t ack = 0;
  bool transmit = false;
  ob->onAppSend(ALICE, 1000, 0, "hi", 2, now, ack, transmit);
  EXPECT_EQ(ob->nextDue(now), now);
  loopAt(now);
  ASSERT_EQ(host.status.size(), 1u);
  EXPECT_EQ(host.status[0].s, UserStatus::QUEUED);
  EXPECT_EQ(ob->nextDue(now), T0 + RDM_WAIT_ACK_MIN_S) << "NEW without transmit times out like WAIT_ACK";
}

TEST_F(OutboxTest, G3_WaitAckTimeoutIsMaxOfTwiceEstAnd30s) {
  send("a");
  EXPECT_EQ(state(), OutState::WAIT_ACK);
  EXPECT_EQ(ob->nextDue(now), T0 + 30);
  expectIdle(T0 + 29);
  loopAt(T0 + 30);
  EXPECT_EQ(state(), OutState::RETRY);
}

TEST_F(OutboxTest, G3_LongEstTimeoutDoubles) {
  host.est_ms = 20000;
  send("b");
  EXPECT_EQ(ob->nextDue(now), T0 + 40);
  expectIdle(T0 + 39);
  loopAt(T0 + 40);
  EXPECT_EQ(state(), OutState::RETRY);
}

TEST_F(OutboxTest, G3_AppRetryRestartsTheTimer) {
  send("a");
  now = T0 + 20;
  send("a", 1000, 1);
  EXPECT_EQ(ob->nextDue(now), T0 + 50);
  expectIdle(T0 + 49);
  loopAt(T0 + 50);
  EXPECT_EQ(state(), OutState::RETRY);

  now = T0 + 70;
  send("a", 1000, 2);
  EXPECT_EQ(state(), OutState::WAIT_ACK) << "app retry in RETRY goes back to WAIT_ACK";
}

// ---- ACK handling ----

TEST_F(OutboxTest, AckRWithCapGivesOnRadioAndSendConfirmedWithAppAck) {
  uint32_t a0 = send("hi");
  now += 5;
  uint32_t a1 = send("hi", 1000, 1);
  now += 5;
  EXPECT_TRUE(sendAck("hi", true, 1000, 0)) << "the ACK of an earlier attempt class matches too";
  EXPECT_EQ(state(), OutState::ON_RADIO);
  ASSERT_EQ(host.confirmed.size(), 1u);
  EXPECT_EQ(host.confirmed[0], a1) << "SEND_CONFIRMED carries the latest app_ack";
  EXPECT_NE(a0, a1);
  EXPECT_EQ(lastStatus(), UserStatus::ON_RADIO);
  EXPECT_TRUE(capOf());
  EXPECT_EQ(entry().deadline, now + RDM_T_SYNC_S);
  EXPECT_TRUE(entry().flags & OF_CONFIRM_SENT);

  EXPECT_TRUE(sendAck("hi", true)) << "a repeated ACK is still ours";
  EXPECT_EQ(host.confirmed.size(), 1u);
}

TEST_F(OutboxTest, AckRWithoutCapGivesOnRadioFinalAndClearsCap) {
  setCap();
  host.rdm_client = false;
  send("hi");
  EXPECT_TRUE(sendAck("hi", false));
  EXPECT_EQ(state(), OutState::ON_RADIO_FINAL);
  EXPECT_FALSE(capOf()) << "G15";
  EXPECT_EQ(host.confirmed.size(), 1u);
  EXPECT_EQ(entry().final_at, now);
}

TEST_F(OutboxTest, AckRWithoutCapInOnRadioMeansStandardFirmwareNow) {
  send("hi");
  host.rdm_client = false;
  sendAck("hi", true);
  sendAck("hi", false);
  EXPECT_EQ(state(), OutState::ON_RADIO_FINAL);
  EXPECT_EQ(host.confirmed.size(), 1u);
}

TEST_F(OutboxTest, AckSFromOnRadioGivesDelivered) {
  send("hi");
  sendAck("hi", true);
  host.rdm_client = false;
  EXPECT_TRUE(sendAckS("hi"));
  EXPECT_EQ(state(), OutState::DELIVERED);
}

TEST_F(OutboxTest, G8_AckSWithoutEverSeeingAckRStillConfirms) {
  send("hi");
  host.rdm_client = false;
  EXPECT_TRUE(sendAckS("hi"));
  EXPECT_EQ(state(), OutState::DELIVERED);
  EXPECT_EQ(host.confirmed.size(), 1u);
  EXPECT_TRUE(capOf()) << "only a fork sends ACK_S";
}

TEST_F(OutboxTest, WrongAckIsIgnored) {
  send("hi");
  uint8_t junk[7] = {1, 2, 3, 4, 5, 6, CAP_BYTE};
  EXPECT_FALSE(ob->onAck(junk, 7, now));
  EXPECT_FALSE(sendAck("other text", true));
  EXPECT_FALSE(sendAck("hi", true, 1001)) << "other timestamp";
  uint8_t shortack[3] = {0};
  ackR(shortack, "hi");
  EXPECT_FALSE(ob->onAck(shortack, 3, now));
  EXPECT_EQ(state(), OutState::WAIT_ACK);
  EXPECT_TRUE(host.confirmed.empty());
}

// ---- RETRY: G16, backoff, attempts, probes, G15 ----

TEST_F(OutboxTest, G16_FirstRetryActionWithoutCapIsDmAfter60To120s) {
  host.rnd = 17;   // 60 + 17 % 61
  send("hi");
  timeout();
  ASSERT_EQ(state(), OutState::RETRY);
  EXPECT_EQ(ob->nextDue(now), T0 + 30 + 77);
  expectIdle(T0 + 30 + 76);
  loopAt(T0 + 30 + 77);
  ASSERT_EQ(host.count(Tx::DM), 1u);
  EXPECT_EQ(host.tx[0].attempt, FW_ATTEMPT_BASE);
  EXPECT_FALSE(host.tx[0].flood);
  EXPECT_EQ(state(), OutState::WAIT_ACK);
}

TEST_F(OutboxTest, G16_JitterSpansExactly60To120s) {
  host.rnd = 60;
  send("a", 1);
  timeout();
  EXPECT_EQ(ob->nextDue(now), now + 120);
  host.rnd = 61;
  send("b", 2);
  timeout();
  EXPECT_EQ(ob->nextDue(now), now + 60);
}

TEST_F(OutboxTest, G16_FirstRetryActionWithCapIsProbe) {
  setCap();
  send("hi");
  timeout();
  uint32_t t = runToTx(T0 + 200, Tx::QUERY);
  EXPECT_EQ(t, T0 + 90);
  EXPECT_EQ(host.count(Tx::DM), 0u);
  const Tx* q = host.last(Tx::QUERY);
  ASSERT_EQ(q->items.size(), 1u);
  QueryItem want = item("hi");
  EXPECT_EQ(q->items[0].ts, want.ts);
  EXPECT_EQ(0, memcmp(q->items[0].key, want.key, 4));
}

TEST_F(OutboxTest, DmBackoffScheduleWithoutCapExactToTheSecond) {
  send("hi");
  timeout();
  runTo(T0 + 3 * DAY + 3 * 3600);
  std::vector<uint32_t> dm = times(host.tx, Tx::DM, T0);
  std::vector<uint32_t> want = {90};
  uint32_t sched[] = RDM_SCHED_DM_BACKOFF;
  for (uint32_t s : sched) want.push_back(want.back() + s);
  want.push_back(want.back() + 86400);
  ASSERT_GE(dm.size(), want.size());
  dm.resize(want.size());
  EXPECT_EQ(dm, want);
  EXPECT_EQ(host.count(Tx::QUERY), 0u) << "no probes without known cap";
}

TEST_F(OutboxTest, FirmwareAttemptsDescendByFourKeepClassAndWrap) {
  setCap();
  send("hi", 1000, 2);
  timeout();
  std::vector<uint8_t> attempts;
  for (int k = 0; k < 64; k++) {
    now += 1;
    reply("hi", QueryState::UNKNOWN);
    runToTx(now + 600, Tx::DM);
    ASSERT_EQ(host.count(Tx::DM), (size_t)k + 1);
    attempts.push_back(host.last(Tx::DM)->attempt);
    runTo(now + RDM_WAIT_ACK_MIN_S);
  }
  for (int k = 0; k < 63; k++) EXPECT_EQ(attempts[k], 254 - 4 * k) << k;
  EXPECT_EQ(attempts[63], 254) << "wraps to 252 + class";
  for (uint8_t a : attempts) EXPECT_EQ(a & 3, 2);
  EXPECT_TRUE(sendAck("hi", true, 1000, 2));
  EXPECT_EQ(state(), OutState::ON_RADIO);
}

TEST_F(OutboxTest, ProbeScheduleWithCapAndG15DailyDm) {
  setCap();
  send("hi");
  timeout();
  runTo(T0 + 2 * DAY + 100);
  std::vector<uint32_t> q = times(host.tx, Tx::QUERY, T0);
  std::vector<uint32_t> dm = times(host.tx, Tx::DM, T0);
  size_t day1 = 0;
  for (uint32_t t : q) day1 += t < DAY;
  EXPECT_EQ(day1, 48u) << "every 30 min in the first 24 h";
  for (size_t k = 0; k < 48; k++) EXPECT_EQ(q[k], 90 + 1800 * k);
  EXPECT_EQ(q[48], 90 + 1800 * 48u);
  EXPECT_EQ(q[49], q[48] + 7200) << "then every 2 h";
  EXPECT_EQ(q[50], q[49] + 7200);
  ASSERT_EQ(dm.size(), 2u) << "G15: a whole DM every 24 h despite the cap";
  EXPECT_EQ(dm[0], DAY);
  EXPECT_EQ(dm[1], 2 * DAY);
}

TEST_F(OutboxTest, FloodWithoutPathAtMostOncePer4h) {
  host.direct = false;
  send("hi");
  timeout();
  runTo(T0 + 6 * 3600);
  std::vector<uint32_t> dm = times(host.tx, Tx::DM, T0);
  ASSERT_EQ(dm.size(), 2u);
  EXPECT_EQ(dm[0], 90u);
  EXPECT_EQ(dm[1], 90u + 300 + 900 + 3600 + 14400) << "the backoff points inside the 4 h are skipped";
  for (const Tx& t : host.tx) EXPECT_TRUE(t.flood);
}

TEST_F(OutboxTest, FloodAfterTwoUnansweredDirectAttempts) {
  send("hi");
  timeout();
  runTo(T0 + 3000);
  ASSERT_GE(host.count(Tx::DM), 3u);
  EXPECT_FALSE(host.tx[0].flood);
  EXPECT_FALSE(host.tx[1].flood);
  EXPECT_TRUE(host.tx[2].flood);
}

TEST_F(OutboxTest, AnswerFromAliceResetsDirectFailures) {
  setCap();
  send("hi");
  timeout();
  runToTx(T0 + 200, Tx::QUERY);
  runToTx(T0 + 2000, Tx::QUERY);
  reply("hi", QueryState::UNKNOWN);
  runToTx(now + 100, Tx::DM);
  EXPECT_FALSE(host.last(Tx::DM)->flood) << "the reply proves the direct path";
}

TEST_F(OutboxTest, OneOutboxTransmissionPer60sOnlyWhenTxIdle) {
  send("a", 1, 0, ALICE);
  send("c", 2, 0, CAROL);
  timeout();
  host.idle = false;
  loopAt(T0 + 90);
  EXPECT_TRUE(host.tx.empty());
  host.idle = true;
  loopAt(T0 + 91);
  ASSERT_EQ(host.tx.size(), 1u);
  EXPECT_EQ(runToTx(T0 + 300, Tx::DM), T0 + 151) << "second contact waits for the 60 s gap";
  ASSERT_EQ(host.tx.size(), 2u);
  EXPECT_NE(host.tx[0].prefix, host.tx[1].prefix);
}

TEST_F(OutboxTest, ProbesAreBundledPerContact) {
  setCap();
  send("one", 1);
  send("two", 2);
  send("three", 3, 0, CAROL);
  timeout();
  runToTx(T0 + 200, Tx::QUERY);
  const Tx* q = host.last(Tx::QUERY);
  EXPECT_EQ(q->prefix, std::vector<uint8_t>(ALICE, ALICE + 6));
  EXPECT_EQ(q->items.size(), 2u);
}

// ---- probe replies (G4) ----

TEST_F(OutboxTest, ProbeUnknownInRetrySendsDmAtOnce) {
  setCap();
  send("hi");
  timeout();
  runToTx(T0 + 200, Tx::QUERY);   // T0 + 90
  now = T0 + 95;
  reply("hi", QueryState::UNKNOWN);
  EXPECT_EQ(ob->nextDue(now), T0 + 150) << "at once, within the 60 s outbox gap";
  loopAt(T0 + 150);
  ASSERT_EQ(host.count(Tx::DM), 1u);
  EXPECT_EQ(host.last(Tx::DM)->attempt, FW_ATTEMPT_BASE);
  EXPECT_EQ(state(), OutState::WAIT_ACK);
}

TEST_F(OutboxTest, ProbeEvictedBeforeOnRadioSendsDm) {
  setCap();
  send("hi");
  timeout();
  runToTx(T0 + 200, Tx::QUERY);
  reply("hi", QueryState::EVICTED);
  runToTx(now + 100, Tx::DM);
  EXPECT_EQ(host.count(Tx::DM), 1u);
}

TEST_F(OutboxTest, G4_ProbeOnRadioWithValidAckR) {
  setCap();
  send("hi");
  timeout();
  uint8_t bad[4] = {9, 9, 9, 9};
  reply("hi", QueryState::ON_RADIO, bad);
  EXPECT_EQ(state(), OutState::RETRY) << "invalid ack leaves the state";
  uint8_t a[4];
  ackR(a, "hi", 1000, 3);
  reply("hi", QueryState::ON_RADIO, a);
  EXPECT_EQ(state(), OutState::ON_RADIO);
  EXPECT_EQ(host.confirmed.size(), 1u);
}

TEST_F(OutboxTest, G4_ProbeSyncedWithValidAckS) {
  setCap();
  host.rdm_client = false;
  send("hi");
  timeout();
  uint8_t r[6];
  ackR(r, "hi");
  r[4] = r[5] = 0;
  reply("hi", QueryState::SYNCED, r);
  EXPECT_EQ(state(), OutState::RETRY) << "ACK_R offered as ACK_S";
  uint8_t s[6];
  ackS(s, "hi");
  reply("hi", QueryState::SYNCED, s);
  EXPECT_EQ(state(), OutState::DELIVERED);
  EXPECT_EQ(host.confirmed.size(), 1u);
}

TEST_F(OutboxTest, QueryReplyForUnknownKeyIsIgnored) {
  setCap();
  send("hi");
  timeout();
  reply("not ours", QueryState::UNKNOWN);
  runTo(T0 + 89);
  EXPECT_EQ(host.count(Tx::DM), 0u);
}

// ---- ON_RADIO ----

TEST_F(OutboxTest, OnRadioReceiptQueriesAfter1h4h12hThenDaily) {
  send("hi");
  sendAck("hi", true);
  std::vector<uint32_t> q = answeredQueries("hi", 5, T0);
  // each answer comes 1 s after its query; the next interval counts from the query itself
  std::vector<uint32_t> want = {3600, 3600 + 14400, 3600 + 14400 + 43200, 3600 + 14400 + 43200 + 86400,
                                3600 + 14400 + 43200 + 2 * 86400};
  EXPECT_EQ(q, want);
  EXPECT_EQ(state(), OutState::ON_RADIO);
}

TEST_F(OutboxTest, OnRadioUnknownFallsBackToRetryAndResends) {
  send("hi");
  sendAck("hi", true);
  runToTx(T0 + 4000, Tx::QUERY);
  size_t pushes = host.status.size();
  reply("hi", QueryState::UNKNOWN);
  EXPECT_EQ(state(), OutState::RETRY);
  ASSERT_EQ(host.status.size(), pushes + 1);
  EXPECT_EQ(lastStatus(), UserStatus::QUEUED) << "G1: fallback reports QUEUED again";
  uint32_t t = now;
  runToTx(now + 100, Tx::DM);
  EXPECT_EQ(now, t + 60) << "DM at once after the gap";
  EXPECT_EQ(host.last(Tx::DM)->attempt, FW_ATTEMPT_BASE);
  sendAck("hi", true);
  EXPECT_EQ(state(), OutState::ON_RADIO);
  EXPECT_EQ(host.confirmed.size(), 1u) << "the tick in the app is not given twice";
}

TEST_F(OutboxTest, OnRadioEvictedOrSyncedGivesDelivered) {
  host.rdm_client = false;
  send("a", 1);
  send("b", 2);
  sendAck("a", true, 1);
  sendAck("b", true, 2);
  reply("a", QueryState::EVICTED, nullptr, 1);
  uint8_t s[6];
  ackS(s, "b", 2);
  reply("b", QueryState::SYNCED, s, 2);
  EXPECT_EQ(state(0), OutState::DELIVERED);
  EXPECT_EQ(state(1), OutState::DELIVERED);
}

TEST_F(OutboxTest, TSyncGivesSyncExpiredThenLateAckSViaReceiptWatch) {
  uint32_t app_ack = send("hi");
  sendAck("hi", true);
  uint32_t on_radio = now;
  runTo(on_radio + RDM_T_SYNC_S - 1);
  EXPECT_EQ(state(), OutState::ON_RADIO);
  loopAt(on_radio + RDM_T_SYNC_S);
  EXPECT_EQ(lastStatus(), UserStatus::SYNC_EXPIRED);
  EXPECT_EQ(ob->count(), 0) << "reported final state frees the slot (G2)";

  now += 40 * DAY;
  EXPECT_TRUE(sendAckS("hi"));
  EXPECT_EQ(lastStatus(), UserStatus::DELIVERED);
  EXPECT_EQ(host.status.back().e.app_ack, app_ack);
  EXPECT_FALSE(sendAckS("hi")) << "watch entry used up";
}

TEST_F(OutboxTest, ReceiptWatchExpiresAfter90Days) {
  send("hi");
  sendAck("hi", true);
  runTo(now + RDM_T_SYNC_S);
  ASSERT_EQ(lastStatus(), UserStatus::SYNC_EXPIRED);
  uint32_t expired_at = now;
  runTo(expired_at + RDM_WATCH_S);
  EXPECT_FALSE(sendAckS("hi"));
}

TEST_F(OutboxTest, ReceiptWatchSurvivesReboot) {
  uint32_t app_ack = send("hi");
  sendAck("hi", true);
  runTo(now + RDM_T_SYNC_S);
  ASSERT_EQ(lastStatus(), UserStatus::SYNC_EXPIRED);
  ASSERT_EQ(ob->count(), 0);
  boot(now + 10 * DAY);
  EXPECT_TRUE(sendAckS("hi")) << "late ACK_S after a reboot";
  EXPECT_EQ(lastStatus(), UserStatus::DELIVERED);
  EXPECT_EQ(host.status.back().e.app_ack, app_ack);
  QueryItem q = item("hi");
  EXPECT_EQ(0, memcmp(host.status.back().e.key, q.key, 4));
  EXPECT_EQ(host.status.back().e.ts, 1000u);
  boot(now + 1);
  EXPECT_FALSE(sendAckS("hi")) << "a used watch record is gone from flash too";
}

TEST_F(OutboxTest, ReceiptWatchExpiryAlsoAppliesAfterReboot) {
  send("hi");
  sendAck("hi", true);
  runTo(now + RDM_T_SYNC_S);
  ASSERT_EQ(lastStatus(), UserStatus::SYNC_EXPIRED);
  boot(now + RDM_WATCH_S);
  EXPECT_EQ(ob->nextDue(now), UINT32_MAX) << "expired record dropped at boot";
  EXPECT_FALSE(sendAckS("hi"));
}

TEST_F(OutboxTest, SyncExpiredEntryStillHeldBecomesDelivered) {
  host.rdm_client = false;
  send("hi");
  sendAck("hi", true);
  runTo(now + RDM_T_SYNC_S);
  ASSERT_EQ(state(), OutState::SYNC_EXPIRED);
  EXPECT_TRUE(sendAckS("hi"));
  EXPECT_EQ(state(), OutState::DELIVERED);
}

// ---- T_radio, G2 slots, G5 ----

TEST_F(OutboxTest, TRadioExpiresRetryExactly) {
  host.rdm_client = false;
  send("hi");
  timeout();
  runTo(T0 + RDM_T_RADIO_S - 1);
  EXPECT_NE(state(), OutState::EXPIRED);
  EXPECT_LE(ob->nextDue(now), T0 + RDM_T_RADIO_S);
  loopAt(T0 + RDM_T_RADIO_S);
  EXPECT_EQ(state(), OutState::EXPIRED);
  EXPECT_EQ(entry().final_at, T0 + RDM_T_RADIO_S);
}

TEST_F(OutboxTest, G2_UnreportedFinalIsFreed24hAfterFinal) {
  host.rdm_client = false;
  send("hi");
  sendAck("hi", false);
  uint32_t fin = now;
  ASSERT_EQ(state(), OutState::ON_RADIO_FINAL);
  EXPECT_TRUE(entry().flags & OF_UNREPORTED);
  EXPECT_EQ(ob->nextDue(now), fin + RDM_FINAL_KEEP_S);
  loopAt(fin + RDM_FINAL_KEEP_S - 1);
  EXPECT_EQ(ob->count(), 1);
  loopAt(fin + RDM_FINAL_KEEP_S);
  EXPECT_EQ(ob->count(), 0);
}

TEST_F(OutboxTest, G2_ReportedFinalIsFreedAtOnce) {
  send("hi");
  sendAck("hi", false);
  EXPECT_EQ(lastStatus(), UserStatus::ON_RADIO_FINAL);
  EXPECT_EQ(ob->count(), 0);
}

TEST_F(OutboxTest, G2_FullOutboxFreesOldestFinalElseNoOutbox) {
  host.rdm_client = false;
  for (uint32_t i = 0; i < 8; i++) {
    now = T0 + i;
    send(("m" + std::to_string(i)).c_str(), 100 + i);
  }
  uint32_t ack = 0;
  bool transmit = false;
  EXPECT_EQ(ob->onAppSend(ALICE, 200, 0, "x", 1, now, ack, transmit), Outbox::SendResult::NO_OUTBOX);
  EXPECT_TRUE(transmit);

  now = T0 + 20;
  sendAck("m5", false, 105);
  now = T0 + 21;
  sendAck("m2", false, 102);
  EXPECT_EQ(ob->onAppSend(ALICE, 200, 0, "x", 1, now, ack, transmit), Outbox::SendResult::NEW_ENTRY);
  bool m5 = false, m2 = false;
  OutEntry e;
  for (uint8_t i = 0; ob->get(i, e); i++) {
    m5 |= e.ts == 105;
    m2 |= e.ts == 102;
  }
  EXPECT_FALSE(m5) << "oldest final entry gave way";
  EXPECT_TRUE(m2);
}

TEST_F(OutboxTest, G5_UnreportedAndConfirmPendingAfterReconnect) {
  host.rdm_client = false;
  host.app = false;
  uint32_t app_ack = send("hi");
  sendAck("hi", true);
  OutEntry e = entry();
  EXPECT_TRUE(e.flags & OF_UNREPORTED);
  EXPECT_TRUE(e.flags & OF_CONFIRM_PENDING);

  host.app = true;
  ob->onClientConnected(false, now);   // standard app: SEND_CONFIRMED only
  ASSERT_EQ(host.confirmed.size(), 1u);
  EXPECT_EQ(host.confirmed[0], app_ack);
  EXPECT_TRUE(host.status.empty());
  e = entry();
  EXPECT_FALSE(e.flags & OF_CONFIRM_PENDING);
  EXPECT_TRUE(e.flags & OF_CONFIRM_SENT);

  host.rdm_client = true;
  ob->onClientConnected(true, now);
  ASSERT_EQ(host.status.size(), 1u);
  EXPECT_EQ(host.status[0].s, UserStatus::ON_RADIO);
  EXPECT_FALSE(entry().flags & OF_UNREPORTED);
  ob->onClientConnected(true, now);
  EXPECT_EQ(host.status.size(), 1u);
  EXPECT_EQ(host.confirmed.size(), 1u);
}

TEST_F(OutboxTest, G5_ReportingAFinalStateOnReconnectFreesTheSlot) {
  host.rdm_client = false;
  send("hi");
  sendAckS("hi");
  ASSERT_EQ(ob->count(), 1);
  host.rdm_client = true;
  ob->onClientConnected(true, now);
  EXPECT_EQ(lastStatus(), UserStatus::DELIVERED);
  EXPECT_EQ(ob->count(), 0);
}

// ---- reboot (G6, Y2) ----

TEST_F(OutboxTest, RebootRecomputesAcksAndKeepsFields) {
  host.rdm_client = false;
  uint32_t a1 = 0;
  send("hi", 1000, 1);
  a1 = entry().app_ack;
  OutEntry before = entry();
  boot(T0 + 500);
  ASSERT_EQ(ob->count(), 1);
  OutEntry after = entry();
  EXPECT_EQ(after.app_ack, a1);
  EXPECT_EQ(0, memcmp(after.key, before.key, 4));
  EXPECT_EQ(after.created, before.created);
  EXPECT_EQ(after.deadline, before.deadline);
  EXPECT_EQ(after.fw_attempt, before.fw_attempt);
  EXPECT_EQ(after.state, OutState::RETRY) << "WAIT_ACK timer did not survive";
  EXPECT_TRUE(sendAck("hi", true, 1000, 1));
  EXPECT_EQ(state(), OutState::ON_RADIO);
  boot(T0 + 600);
  EXPECT_TRUE(sendAckS("hi"));
  EXPECT_EQ(state(), OutState::DELIVERED);
}

TEST_F(OutboxTest, G6_BootKickRoundAfter60To120sThenSchedule) {
  host.rdm_client = false;
  send("radio", 1, 0, ALICE);
  sendAck("radio", true, 1);
  send("retry", 2, 0, CAROL);
  timeout();
  runTo(T0 + 5000);
  size_t before = host.tx.size();
  host.rnd = 30;
  boot(T0 + 6000);
  uint32_t kick = T0 + 6000 + 90;
  EXPECT_EQ(ob->nextDue(now), kick);
  expectIdle(kick - 1);
  loopAt(kick);
  loopAt(kick + 60);
  ASSERT_EQ(host.tx.size(), before + 2);
  std::vector<Tx::Kind> kinds = {host.tx[before].kind, host.tx[before + 1].kind};
  EXPECT_NE(std::find(kinds.begin(), kinds.end(), Tx::QUERY), kinds.end()) << "ON_RADIO: receipt query";
  EXPECT_NE(std::find(kinds.begin(), kinds.end(), Tx::DM), kinds.end()) << "RETRY without cap: DM";
  EXPECT_EQ(host.tx[before + 1].at, kick + 60);
}

TEST_F(OutboxTest, BootPersistsOnlyRecordsWhoseStateChanged) {
  host.rdm_client = false;
  send("radio", 1);
  sendAck("radio", true, 1);              // ON_RADIO: a boot leaves the record as it is
  toCustody("custody");                   // CUSTODY: idem
  uint32_t w0 = io.writeCalls();
  boot(now + 100);
  EXPECT_EQ(io.writeCalls(), w0) << "unchanged records are not rewritten";
  EXPECT_EQ(state(0), OutState::ON_RADIO);
  EXPECT_EQ(state(1), OutState::CUSTODY);

  send("wait", 3);                        // WAIT_ACK: RETRY after a boot, so this one record is rewritten
  uint32_t a = 0;
  bool transmit = false;
  w0 = io.writeCalls();
  ASSERT_EQ(ob->onAppSend(ALICE, 3, 1, "wait", 4, now, a, transmit), Outbox::SendResult::EXISTING_ENTRY);
  uint32_t one_record = io.writeCalls() - w0;   // a new app_ack is exactly one persist
  ASSERT_GT(one_record, 0u);
  w0 = io.writeCalls();
  boot(now + 200);
  EXPECT_EQ(io.writeCalls() - w0, one_record);
  EXPECT_EQ(state(2), OutState::RETRY);
}

TEST_F(OutboxTest, G6_OnRadioScheduleResumesFromItsAnchorAfterReboot) {
  send("hi");
  sendAck("hi", true);
  uint32_t on_radio = now;
  answeredQueries("hi", 1, on_radio);   // first query at +1 h
  boot(on_radio + 2 * 3600);
  std::vector<uint32_t> q = answeredQueries("hi", 2, on_radio + 2 * 3600);
  ASSERT_EQ(q.size(), 2u);
  EXPECT_EQ(q[1] - q[0], 43200u) << "the kick replaced the +4 h query, the schedule goes on at +12 h";
}

// ---- mailbox: DEPOSITING, REGISTER, CUSTODY (G9, G10, G13) ----

TEST_F(OutboxTest, WaitAckTimeoutWithMailboxDeposits) {
  setMailbox(MBX1);
  host.rdm_client = true;
  send("hi");
  timeout();
  EXPECT_EQ(state(), OutState::DEPOSITING);
  ASSERT_EQ(host.count(Tx::DEPOSIT), 1u);
  const Tx* d = host.last(Tx::DEPOSIT);
  EXPECT_EQ(d->attempt, FW_ATTEMPT_BASE);
  EXPECT_EQ(lastStatus(), UserStatus::QUEUED);

  uint8_t wrong[8] = {1};
  ob->onDepositReply(MbxCode::OK, wrong, DAY, now);
  EXPECT_EQ(state(), OutState::DEPOSITING) << "hash must match";

  ob->onDepositReply(MbxCode::OK, d->hash.data(), 10 * DAY, now);
  OutEntry e = entry();
  EXPECT_EQ(e.state, OutState::CUSTODY);
  EXPECT_TRUE(e.flags & OF_MBX_COPY);
  EXPECT_EQ(0, memcmp(e.pkt_hash, d->hash.data(), 8));
  EXPECT_EQ(e.deadline, now + 10 * DAY + RDM_CUSTODY_GRACE_S) << "G10";
  EXPECT_EQ(lastStatus(), UserStatus::CUSTODY);
  EXPECT_TRUE(host.confirmed.empty()) << "no tick for custody";
}

TEST_F(OutboxTest, G10_CustodyDeadlineCappedAt30Days) {
  toCustody("hi", 90 * DAY);
  EXPECT_EQ(entry().deadline, now + 30 * DAY + RDM_CUSTODY_GRACE_S);
}

TEST_F(OutboxTest, AlreadyStoredCountsAsStored) {
  setMailbox(MBX1);
  send("hi");
  timeout();
  ob->onDepositReply(MbxCode::ALREADY_STORED, host.last(Tx::DEPOSIT)->hash.data(), DAY, now);
  EXPECT_EQ(state(), OutState::CUSTODY);
}

TEST_F(OutboxTest, TextAboveMailboxLimitIsNotDeposited) {
  setMailbox(MBX1);
  std::string t(MAX_TEXT_MAILBOX + 1, 'y');
  send(t.c_str());
  timeout();
  EXPECT_EQ(state(), OutState::RETRY);
  runTo(T0 + DAY);
  EXPECT_EQ(host.count(Tx::DEPOSIT), 0u);
  t.resize(MAX_TEXT_MAILBOX);
  send(t.c_str(), 2);
  timeout();
  EXPECT_EQ(state(1), OutState::DEPOSITING);
}

TEST_F(OutboxTest, G9_NotAuthRegistersOnceThenDepositsSamePayload) {
  setMailbox(MBX1);
  send("hi");
  timeout();
  std::vector<uint8_t> h1 = host.last(Tx::DEPOSIT)->hash;
  ob->onDepositReply(MbxCode::NOT_AUTH, h1.data(), 0, now);
  EXPECT_EQ(state(), OutState::REGISTER);
  uint32_t t = runToTx(now + 100, Tx::REGISTER);
  EXPECT_EQ(t, T0 + 90) << "after the 60 s gap";
  ob->onRegisterReply(ALICE, MbxCode::OK, now);
  EXPECT_EQ(state(), OutState::DEPOSITING);
  runToTx(now + 100, Tx::DEPOSIT);
  EXPECT_EQ(host.last(Tx::DEPOSIT)->hash, h1) << "same attempt, so M can answer ALREADY_STORED";
  ob->onDepositReply(MbxCode::NOT_AUTH, h1.data(), 0, now);
  EXPECT_EQ(state(), OutState::RETRY) << "max one REGISTER per round";
  EXPECT_EQ(host.count(Tx::REGISTER), 1u);
}

TEST_F(OutboxTest, G9_RegisterRefusedOrSilentGoesToRetry) {
  setMailbox(MBX1);
  send("hi");
  timeout();
  ob->onDepositReply(MbxCode::NOT_AUTH, host.last(Tx::DEPOSIT)->hash.data(), 0, now);
  runToTx(now + 100, Tx::REGISTER);
  ob->onRegisterReply(ALICE, MbxCode::NOT_AUTH, now);
  EXPECT_EQ(state(), OutState::RETRY);

  send("second", 2);
  timeout();
  ob->onDepositReply(MbxCode::NOT_AUTH, host.last(Tx::DEPOSIT)->hash.data(), 0, now);
  runToTx(now + 100, Tx::REGISTER);
  uint32_t sent = now;
  runTo(sent + RDM_WAIT_ACK_MIN_S);
  EXPECT_EQ(state(1), OutState::RETRY) << "no answer to REGISTER";
}

TEST_F(OutboxTest, G9_TwoDepositsWithoutReplyTriggerRegister) {
  setMailbox(MBX1);
  send("hi");
  timeout();   // DEPOSIT at T0 + 30
  loopAt(T0 + 60);
  EXPECT_EQ(state(), OutState::RETRY) << "first silence: RETRY";
  uint32_t t = runToTx(T0 + 2000, Tx::DEPOSIT);
  EXPECT_EQ(t, T0 + 60 + 600) << "G13 re-deposit after 10 min";
  runTo(t + 30);
  EXPECT_EQ(state(), OutState::REGISTER) << "second silence in a row: cache miss";
  runToTx(now + 100, Tx::REGISTER);
  EXPECT_EQ(host.count(Tx::REGISTER), 1u);
}

TEST_F(OutboxTest, FailedDepositsGoToRetryAndRedepositOnSchedule) {
  setMailbox(MBX1);
  setCap();
  host.direct = true;
  send("hi");
  timeout();
  std::vector<uint32_t> fail_at;
  std::vector<uint32_t> dep_at;
  MbxCode codes[] = {MbxCode::NO_STORAGE, MbxCode::QUOTA, MbxCode::RATE_LIMITED, MbxCode::UNKNOWN_OWNER,
                     MbxCode::NO_STORAGE, MbxCode::NO_STORAGE};
  for (MbxCode c : codes) {
    ob->onDepositReply(c, host.last(Tx::DEPOSIT)->hash.data(), 0, now);
    EXPECT_EQ(state(), OutState::RETRY);
    fail_at.push_back(now);
    runToTx(now + 2 * DAY, Tx::DEPOSIT);
    dep_at.push_back(now);
  }
  uint32_t sched[] = RDM_SCHED_REDEPOSIT;
  for (size_t k = 0; k < 6; k++) {
    uint32_t want = sched[k < 5 ? k : 4];
    EXPECT_GE(dep_at[k] - fail_at[k], want) << k;
    EXPECT_LT(dep_at[k] - fail_at[k], want + 60) << "at most the outbox gap later";
  }
}

TEST_F(OutboxTest, G13_MailboxAdvertTriggersRedepositAtMostEvery10Min) {
  setMailbox(MBX1);
  send("hi");
  timeout();
  ob->onDepositReply(MbxCode::NO_STORAGE, host.last(Tx::DEPOSIT)->hash.data(), 0, now);
  runTo(now + 200);
  size_t n = host.count(Tx::DEPOSIT);
  ob->onMailboxAdvert(MBX1, now);
  runToTx(now + 100, Tx::DEPOSIT);
  EXPECT_EQ(host.count(Tx::DEPOSIT), n + 1);
  ob->onDepositReply(MbxCode::NO_STORAGE, host.last(Tx::DEPOSIT)->hash.data(), 0, now);
  uint32_t t = now + 100;
  runTo(t);
  ob->onMailboxAdvert(MBX1, now);
  runTo(t + 120);
  EXPECT_EQ(host.count(Tx::DEPOSIT), n + 1) << "second advert within 10 min ignored";
  uint8_t other[6] = {0x99};
  ob->onMailboxAdvert(other, now);
  runTo(now + 100);
  EXPECT_EQ(host.count(Tx::DEPOSIT), n + 1);
}

TEST_F(OutboxTest, TooBigFromMailboxStopsDeposits) {
  setMailbox(MBX1);
  send("hi");
  timeout();
  ob->onDepositReply(MbxCode::TOO_BIG, host.last(Tx::DEPOSIT)->hash.data(), 0, now);
  EXPECT_EQ(state(), OutState::RETRY);
  runTo(now + DAY);
  EXPECT_EQ(host.count(Tx::DEPOSIT), 1u);
}

TEST_F(OutboxTest, LateStoredAfterGivingUpStillGivesCustody) {
  setMailbox(MBX1);
  send("hi");
  timeout();
  std::vector<uint8_t> h = host.last(Tx::DEPOSIT)->hash;
  loopAt(now + 30);
  ASSERT_EQ(state(), OutState::RETRY);
  ob->onDepositReply(MbxCode::OK, h.data(), DAY, now);
  EXPECT_EQ(state(), OutState::CUSTODY);
}

TEST_F(OutboxTest, CustodyStatusScheduleAndAdvertTrigger) {
  uint32_t c = toCustody("hi");
  std::vector<uint32_t> st = answeredStatuses(6, c);
  std::vector<uint32_t> want = {600, 600 + 1800, 600 + 1800 + 3600, 600 + 1800 + 3600 + 14400,
                                600 + 1800 + 3600 + 14400 + 43200, 600 + 1800 + 3600 + 14400 + 2 * 43200};
  EXPECT_EQ(st, want);
  EXPECT_EQ(times(host.tx, Tx::QUERY, c).at(0), DAY) << "probe to Alice every 24 h in CUSTODY";
  now += 100;

  size_t n = host.count(Tx::STATUS);
  ob->onMailboxAdvert(MBX1, now);
  runToTx(now + 100, Tx::STATUS);
  EXPECT_EQ(host.count(Tx::STATUS), n + 1);
  statusReply(MbxState::STORED);
  ob->onMailboxAdvert(MBX1, now + 300);
  runTo(now + 400);
  EXPECT_EQ(host.count(Tx::STATUS), n + 1);
  const Tx* s = host.last(Tx::STATUS);
  ASSERT_EQ(s->hashes.size(), 1u);
  EXPECT_EQ(0, memcmp(s->hashes[0].data(), entry().pkt_hash, 8));
}

TEST_F(OutboxTest, CustodyStatusOnRadioWithValidAckR) {
  toCustody("hi");
  uint8_t bad[6] = {1, 2, 3, 4, 0, 0};
  statusReply(MbxState::ON_RADIO, bad, 6);
  EXPECT_EQ(state(), OutState::CUSTODY);
  uint8_t a[6] = {0};
  ackR(a, "hi", 1000, 0);
  statusReply(MbxState::ON_RADIO, a, 6);
  EXPECT_EQ(state(), OutState::ON_RADIO);
  EXPECT_EQ(host.confirmed.size(), 1u);
}

TEST_F(OutboxTest, G8_CustodyStatusDeliveredWithValidAckS) {
  host.rdm_client = false;
  toCustody("hi");
  uint8_t r[6] = {0};
  ackR(r, "hi");
  statusReply(MbxState::DELIVERED, r, 6);
  EXPECT_EQ(state(), OutState::CUSTODY) << "mailbox offers ACK_R as ACK_S";
  uint8_t s[6];
  ackS(s, "hi");
  statusReply(MbxState::DELIVERED, s, 6);
  EXPECT_EQ(state(), OutState::DELIVERED);
  EXPECT_EQ(host.confirmed.size(), 1u) << "G8: SEND_CONFIRMED although ACK_R was never seen";
}

// R5: in ON_RADIO too, only Alice's ACK_S makes a mailbox's DELIVERED count; M knows ACK_R from her report.
TEST_F(OutboxTest, OnRadioStatusDeliveredWithoutValidAckSIsIgnored) {
  toCustody("hi");
  uint8_t r[6] = {0};
  ackR(r, "hi");
  statusReply(MbxState::ON_RADIO, r, 6);
  ASSERT_EQ(state(), OutState::ON_RADIO);
  statusReply(MbxState::DELIVERED, r, 6);
  EXPECT_EQ(state(), OutState::ON_RADIO) << "mailbox offers ACK_R as ACK_S";
  uint8_t junk[6] = {1, 2, 3, 4, 5, 6};
  statusReply(MbxState::DELIVERED, junk, 6);
  EXPECT_EQ(state(), OutState::ON_RADIO) << "mailbox offers random bytes as ACK_S";
  host.rdm_client = false;
  uint8_t s[6];
  ackS(s, "hi");
  statusReply(MbxState::DELIVERED, s, 6);
  EXPECT_EQ(state(), OutState::DELIVERED);
}

TEST_F(OutboxTest, CustodyStatusUnknownGoesToRetryAndRejectedIsFinal) {
  host.rdm_client = false;
  toCustody("a");
  statusReply(MbxState::UNKNOWN);
  EXPECT_EQ(state(), OutState::RETRY);
  EXPECT_FALSE(entry().flags & OF_MBX_COPY);

  ob.reset();
  io.wipeAll();
  boot(now);
  toCustody("b");
  statusReply(MbxState::STORED);
  EXPECT_EQ(state(), OutState::CUSTODY);
  statusReply(MbxState::REJECTED);
  EXPECT_EQ(state(), OutState::REJECTED);
}

TEST_F(OutboxTest, FallbackFromCustodyGetsAtLeastOneMoreDay) {
  host.rdm_client = false;
  toCustody("late", 20 * DAY);
  runTo(T0 + 15 * DAY);
  statusReply(MbxState::UNKNOWN);
  ASSERT_EQ(state(), OutState::RETRY);
  EXPECT_EQ(entry().deadline, now + DAY) << "created + T_radio lies in the past";
  runTo(now + DAY - 1);
  EXPECT_EQ(state(), OutState::RETRY);
  runTo(now + 1);
  EXPECT_EQ(state(), OutState::EXPIRED);
}

TEST_F(OutboxTest, EarlyFallbackFromCustodyKeepsTRadio) {
  toCustody("early");
  now += 3600;
  statusReply(MbxState::UNKNOWN);
  ASSERT_EQ(state(), OutState::RETRY);
  EXPECT_EQ(entry().deadline, T0 + RDM_T_RADIO_S);
}

TEST_F(OutboxTest, FallbackFromOnRadioGetsAtLeastOneMoreDay) {
  send("hi");
  sendAck("hi", true);
  runTo(T0 + 10 * DAY);
  reply("hi", QueryState::UNKNOWN);
  ASSERT_EQ(state(), OutState::RETRY);
  EXPECT_EQ(entry().deadline, now + DAY);
}

TEST_F(OutboxTest, CustodyProbeUnknownSendsDirectDmAndStaysCustody) {
  uint32_t c = toCustody("hi");
  runToTx(c + DAY + 100, Tx::QUERY);
  EXPECT_EQ(now, c + DAY);
  reply("hi", QueryState::UNKNOWN);
  runToTx(now + 100, Tx::DM);
  EXPECT_EQ(state(), OutState::CUSTODY) << "I4";
  EXPECT_NE(host.last(Tx::DM)->attempt, host.last(Tx::DEPOSIT)->attempt) << "own firmware attempt";
  sendAck("hi", true);
  EXPECT_EQ(state(), OutState::ON_RADIO);
}

TEST_F(OutboxTest, G8_CustodyProbeOnRadioOrSyncedSkipsTheMailbox) {
  host.rdm_client = false;
  toCustody("a", 10 * DAY);
  uint8_t r[4];
  ackR(r, "a");
  reply("a", QueryState::ON_RADIO, r);
  EXPECT_EQ(state(), OutState::ON_RADIO);
  EXPECT_EQ(host.confirmed.size(), 1u);

  io.wipeAll();
  boot(now);
  toCustody("b", 10 * DAY);
  uint8_t s[6];
  ackS(s, "b");
  reply("b", QueryState::SYNCED, s);
  EXPECT_EQ(state(), OutState::DELIVERED);
}

TEST_F(OutboxTest, CustodyHeardTriggersProbeAtMostHourly) {
  uint32_t c = toCustody("hi");
  runTo(c + 700);
  size_t q = host.count(Tx::QUERY);
  ob->onHeard(ALICE, true, now);
  runToTx(now + 100, Tx::QUERY);
  EXPECT_EQ(host.count(Tx::QUERY), q + 1);
  ob->onHeard(ALICE, true, now + 600);
  runTo(now + 1200);
  EXPECT_EQ(host.count(Tx::QUERY), q + 1);
}

TEST_F(OutboxTest, G10_CustodyExpiresOnlyAfterDeadlineAndFinalStatus) {
  host.rdm_client = false;
  uint32_t c = toCustody("hi", 2 * DAY);
  uint32_t deadline = c + 3 * DAY;
  runTo(deadline - 10);
  statusReply(MbxState::EXPIRED);
  EXPECT_EQ(state(), OutState::CUSTODY) << "not before expires + grace";
  runToTx(deadline + 100, Tx::STATUS);
  // the daily probe to Alice falls on the same second here and takes the first outbox slot
  EXPECT_GE(now, deadline);
  EXPECT_LE(now, deadline + RDM_OUTBOX_TX_GAP_S) << "final STATUS at the deadline";
  EXPECT_EQ(state(), OutState::CUSTODY);
  statusReply(MbxState::EXPIRED);
  EXPECT_EQ(state(), OutState::EXPIRED);
}

TEST_F(OutboxTest, G10_SilentOrUnknownFinalStatusExpires) {
  host.rdm_client = false;
  uint32_t c = toCustody("a", DAY);
  runToTx(c + 2 * DAY + 100, Tx::STATUS);
  while (now < c + 2 * DAY) runToTx(c + 2 * DAY + 100, Tx::STATUS);
  uint32_t final_at = now;
  runTo(final_at + RDM_WAIT_ACK_MIN_S);
  EXPECT_EQ(state(), OutState::CUSTODY) << "G17b: one repeat first";
  runToTx(now + 200, Tx::STATUS);
  EXPECT_EQ(now, final_at + RDM_WAIT_ACK_MIN_S + RDM_RETRY_FIRST_MIN_S);
  runTo(now + RDM_WAIT_ACK_MIN_S);
  EXPECT_EQ(state(), OutState::EXPIRED);

  io.wipeAll();
  boot(now);
  c = toCustody("b", DAY);
  while (now < c + 2 * DAY) runToTx(c + 2 * DAY + 100, Tx::STATUS);
  statusReply(MbxState::UNKNOWN);
  EXPECT_EQ(state(), OutState::EXPIRED);
}

TEST_F(OutboxTest, G11_OnRadioWithMailboxCopyUsesStatusAndQueriesOnlyWhenHeard) {
  uint32_t c = toCustody("hi");
  uint8_t a[6] = {0};
  ackR(a, "hi");
  now = c + 100;
  statusReply(MbxState::ON_RADIO, a, 6);
  ASSERT_EQ(state(), OutState::ON_RADIO);
  size_t q = host.count(Tx::QUERY);
  uint32_t t = runToTx(now + DAY, Tx::STATUS);
  EXPECT_EQ(t, c + 100 + 3600);
  runTo(now + DAY);
  EXPECT_EQ(host.count(Tx::QUERY), q) << "no RECEIPT_QUERY by schedule";
  ob->onHeard(ALICE, true, now);
  runToTx(now + 100, Tx::QUERY);
  EXPECT_EQ(host.count(Tx::QUERY), q + 1);
  host.rdm_client = false;
  uint8_t s[6];
  ackS(s, "hi");
  statusReply(MbxState::DELIVERED, s, 6);
  EXPECT_EQ(state(), OutState::DELIVERED);
}

TEST_F(OutboxTest, G14_NewMailboxMovesCustodyToDepositAtNewMailbox) {
  toCustody("hi");
  const Tx first = *host.last(Tx::DEPOSIT);
  setMailbox(MBX2);
  ob->onMailboxChanged(ALICE, now);
  EXPECT_EQ(state(), OutState::DEPOSITING);
  EXPECT_FALSE(entry().flags & OF_MBX_COPY);
  runToTx(now + 100, Tx::DEPOSIT);
  const Tx* d = host.last(Tx::DEPOSIT);
  EXPECT_NE(d->attempt, first.attempt) << "new firmware attempt";
  EXPECT_NE(d->hash, first.hash) << "own pkt_hash";
  ob->onDepositReply(MbxCode::OK, first.hash.data(), DAY, now);
  EXPECT_EQ(state(), OutState::DEPOSITING) << "reply for the old copy does not count";
  ob->onDepositReply(MbxCode::OK, d->hash.data(), DAY, now);
  EXPECT_EQ(state(), OutState::CUSTODY);
}

TEST_F(OutboxTest, G14_LateMailboxChangeGetsAtLeastOneMoreDay) {
  toCustody("hi", 20 * DAY);
  runTo(T0 + 15 * DAY);
  setMailbox(MBX2);
  ob->onMailboxChanged(ALICE, now);
  ASSERT_EQ(state(), OutState::DEPOSITING);
  EXPECT_EQ(entry().deadline, now + DAY) << "old ttl_s no longer applies, T_radio has passed";
}

TEST_F(OutboxTest, G14_RevokeMovesCustodyToRetry) {
  toCustody("hi");
  setMailbox(nullptr);
  ob->onMailboxChanged(ALICE, now);
  EXPECT_EQ(state(), OutState::RETRY);
  runTo(now + DAY);
  EXPECT_EQ(host.count(Tx::DEPOSIT), 1u);
  EXPECT_GE(host.count(Tx::DM), 1u);
}

// ---- G17: jitter on repeating intervals, one repeat of an unanswered STATUS/query ----

namespace {
// r % 31 == 30 and r % 61 == 60: the largest draw for J = 30 (300 s) and J = 60 (600 s and up)
const uint32_t RND_MAX_J = 31 * 61 - 1;
uint32_t jitterOf(uint32_t i) { return i / RDM_JITTER_DIV < RDM_JITTER_MAX_S ? i / RDM_JITTER_DIV : RDM_JITTER_MAX_S; }
}

TEST_F(OutboxTest, G17_LargestDrawGivesIntervalPlusJ) {
  host.rnd = RND_MAX_J;
  send("hi");
  timeout();
  runTo(T0 + DAY);
  std::vector<uint32_t> dm = times(host.tx, Tx::DM, T0);
  ASSERT_GE(dm.size(), 4u);
  EXPECT_EQ(dm[0], 30u + RDM_RETRY_FIRST_MAX_S) << "G16 keeps its own 60-120 s";
  uint32_t sched[] = RDM_SCHED_DM_BACKOFF;
  for (size_t k = 1; k < dm.size(); k++) {
    uint32_t i = sched[k - 1 < 5 ? k - 1 : 5];
    EXPECT_EQ(dm[k] - dm[k - 1], i + jitterOf(i)) << k;
  }
}

TEST_F(OutboxTest, G17_AnyDrawStaysWithinIntervalAndJ) {
  const uint32_t draws[] = {0xFFFFFFFFu, 12345, 1};
  for (uint32_t r : draws) {
    io.wipeAll();
    host.tx.clear();
    host.rnd = r;
    boot(T0);
    send("hi");
    sendAck("hi", true);
    std::vector<uint32_t> q = answeredQueries("hi", 4, T0);
    ASSERT_EQ(q.size(), 4u);
    uint32_t sched[] = RDM_SCHED_ON_RADIO;
    uint32_t prev = 0;
    for (size_t k = 0; k < q.size(); k++) {
      uint32_t d = q[k] - prev;
      EXPECT_GE(d, sched[k]) << "draw " << r << " step " << k;
      EXPECT_LE(d, sched[k] + jitterOf(sched[k])) << "draw " << r << " step " << k;
      prev = q[k];
    }
  }
}

TEST_F(OutboxTest, G17_SchedulesWithVaryingDrawsStayInRange) {
  host.lcg = true;
  host.seq.s = 7;
  uint32_t c = toCustody("hi");
  std::vector<uint32_t> st = answeredStatuses(7, c);
  uint32_t sched[] = RDM_SCHED_STATUS;
  uint32_t prev = 0;
  bool varied = false;
  for (size_t k = 0; k < st.size(); k++) {
    uint32_t i = sched[k < 5 ? k : 4];
    uint32_t d = st[k] - prev;
    EXPECT_GE(d, i) << k;
    EXPECT_LE(d, i + jitterOf(i) + 1) << k;
    varied |= d != i;
    prev = st[k];
  }
  EXPECT_TRUE(varied);
}

TEST_F(OutboxTest, G17_NoJitterOnTimeoutsAndDeadlines) {
  host.rnd = RND_MAX_J;
  host.rdm_client = false;
  send("a");
  EXPECT_EQ(ob->nextDue(now), T0 + RDM_WAIT_ACK_MIN_S) << "WAIT_ACK";
  sendAck("a", true);
  runTo(T0 + RDM_T_SYNC_S - 1);
  EXPECT_EQ(state(), OutState::ON_RADIO);
  loopAt(T0 + RDM_T_SYNC_S);
  EXPECT_EQ(state(), OutState::SYNC_EXPIRED) << "T_sync exact";
  EXPECT_EQ(ob->nextDue(now), now + RDM_FINAL_KEEP_S) << "final keep exact";

  io.wipeAll();
  boot(T0);
  send("b");
  timeout();
  runTo(T0 + RDM_T_RADIO_S - 1);
  EXPECT_NE(state(), OutState::EXPIRED);
  loopAt(T0 + RDM_T_RADIO_S);
  EXPECT_EQ(state(), OutState::EXPIRED) << "T_radio exact";
}

TEST_F(OutboxTest, G17_UnansweredStatusIsRepeatedOnceKeepingTheSchedule) {
  uint32_t c = toCustody("hi");
  runToTx(c + 700, Tx::STATUS);
  ASSERT_EQ(now, c + 600);
  uint32_t first = now;
  uint32_t t = runToTx(first + 300, Tx::STATUS);
  EXPECT_EQ(t, first + RDM_WAIT_ACK_MIN_S + RDM_RETRY_FIRST_MIN_S) << "one repeat 60-120 s after the time-out";
  runTo(first + 1799);
  EXPECT_EQ(host.count(Tx::STATUS), 2u) << "the repeat is not repeated";
  t = runToTx(first + 2000, Tx::STATUS);
  EXPECT_EQ(t, first + 1800) << "next step from the original STATUS";
}

TEST_F(OutboxTest, G17_RepeatDelayIsSixtyToOneTwentySeconds) {
  host.rnd = RND_MAX_J;
  uint32_t c = toCustody("hi");
  uint32_t first = runToTx(c + 1000, Tx::STATUS);
  uint32_t t = runToTx(first + 400, Tx::STATUS);
  EXPECT_EQ(t, first + RDM_WAIT_ACK_MIN_S + RDM_RETRY_FIRST_MAX_S);
}

TEST_F(OutboxTest, G17_AnsweredStatusIsNotRepeated) {
  uint32_t c = toCustody("hi");
  runToTx(c + 700, Tx::STATUS);
  now += 2;
  statusReply(MbxState::STORED);
  runTo(c + 1799);
  EXPECT_EQ(host.count(Tx::STATUS), 1u);
}

TEST_F(OutboxTest, G17_UnansweredOnRadioQueryIsRepeatedOnce) {
  send("hi");
  sendAck("hi", true);
  uint32_t first = runToTx(T0 + 4000, Tx::QUERY);
  ASSERT_EQ(first, T0 + 3600);
  uint32_t t = runToTx(first + 300, Tx::QUERY);
  EXPECT_EQ(t, first + RDM_WAIT_ACK_MIN_S + RDM_RETRY_FIRST_MIN_S);
  runTo(first + 14399);
  EXPECT_EQ(host.count(Tx::QUERY), 2u) << "the repeat is not repeated";
  t = runToTx(first + 15000, Tx::QUERY);
  EXPECT_EQ(t, first + 14400) << "next step from the original query";
}

TEST_F(OutboxTest, G17_UnansweredProbeInRetryIsNotRepeated) {
  setCap();
  send("hi");
  timeout();
  runTo(T0 + 90 + 1799);
  EXPECT_EQ(host.count(Tx::QUERY), 1u) << "silence in RETRY is normal: Alice is offline";
  uint32_t c0 = host.count(Tx::QUERY);
  uint32_t t = runToTx(T0 + 5000, Tx::QUERY);
  EXPECT_EQ(t, T0 + 90 + 1800);
  EXPECT_EQ(host.count(Tx::QUERY), c0 + 1);

  io.wipeAll();
  host.tx.clear();
  boot(now);
  uint32_t c = toCustody("x");
  size_t q = host.count(Tx::QUERY);
  runTo(c + DAY + 1000);
  EXPECT_EQ(host.count(Tx::QUERY), q + 1) << "CUSTODY probe: no repeat either";
}

TEST_F(OutboxTest, G17_TwoNodesStartingTogetherDoNotStayInStep) {
  // Bob's STATUS schedule on two nodes with the same start but other random draws
  FakeHost other;
  other.rnd = 17;
  MemFileIO io2;
  RecordFile f2(io2, Outbox::PATH, 1, Outbox::RECORD_SIZE, 8, true),
      w2(io2, Outbox::WATCH_PATH, 1, Outbox::WATCH_RECORD_SIZE, 32, false),
      c2(io2, ContactTable::PATH, 1, ContactTable::RECORD_SIZE, 16, false);
  ASSERT_NE(c2.open(), RecordFile::Open::FAILED);
  ContactTable ct2(c2);
  ASSERT_TRUE(ct2.begin());
  Outbox ob2(f2, w2, ct2, other);
  ASSERT_TRUE(ob2.begin(T0));

  send("hi");
  sendAck("hi", true);
  uint32_t ack = 0;
  bool transmit = false;
  ob2.onAppSend(ALICE, 1000, 0, "hi", 2, T0, ack, transmit);
  ob2.onAppTransmitted(ALICE, 1000, 5000, T0);
  uint8_t a[7];
  crypto::ackR(a, 1000, 0, "hi", 2, other.self);
  a[4] = 252;
  a[5] = 1;
  a[6] = CAP_BYTE;
  ASSERT_TRUE(ob2.onAck(a, 7, T0));
  EXPECT_EQ(ob->nextDue(T0), T0 + 3600);
  EXPECT_EQ(ob2.nextDue(T0), T0 + 3617);
}

// ---- loop() idle cache (D37) ----

namespace {
const uint8_t NOBODY[6] = {0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE};

struct Trace {
  std::vector<std::string> tx, status;
  std::vector<uint32_t> confirmed;
  std::vector<OutState> states;
  uint32_t idle_calls;
};
}

// The cache never moves a transmission, a push or a state change to a later second. The reference run drops the
// cache before every loop() with a mutator that changes nothing (an advert of an unknown mailbox); the same
// scripted host events, replies and blocked periods happen in both runs, so the seeded random draws must line up.
TEST_F(OutboxTest, NextWakeupIsNeverLate) {
  auto run = [&](bool drop_cache) -> Trace {
    io.wipeAll();
    host.tx.clear();
    host.status.clear();
    host.confirmed.clear();
    host.idle_calls = 0;
    host.contact = host.direct = host.idle = host.send_ok = true;
    host.lcg = true;
    host.seq.s = 4711;
    boot(T0);
    setCap(CAROL);
    const uint8_t DAVE[6] = {0xD1, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6};
    setMailbox(MBX1, DAVE);
    send("to alice", 1);
    send("probe carol", 2, 0, CAROL);
    send("deposit dave", 3, 0, DAVE);
    const uint32_t end = T0 + 3 * DAY;
    for (uint32_t t = T0 + 1; t <= end; t++) {
      now = host.now = t;
      host.idle = !(t >= T0 + 100 && t < T0 + 400);        // TX queue busy while the first actions are due
      host.contact = !(t >= T0 + 3000 && t < T0 + 3300);   // contact lookup fails around a DM
      host.direct = t < T0 + DAY / 2;                       // path lost after half a day: floods
      if (t == T0 + 5000) sendAck("to alice", true, 1);    // alice: ON_RADIO, receipt queries from now on
      if (t == T0 + 200) {
        const Tx* d = host.last(Tx::DEPOSIT);
        if (d) ob->onDepositReply(MbxCode::OK, d->hash.data(), 2 * DAY, now);
      }
      if (t == T0 + DAY + 700) statusReply(MbxState::STORED, nullptr, 0, 2);
      if (t == T0 + 2 * DAY + 100) ob->onHeard(CAROL, true, now);
      if (t == T0 + 2 * DAY + 5000) ob->onMailboxAdvert(MBX1, now);
      if (drop_cache) ob->onMailboxAdvert(NOBODY, now);
      ob->loop(now);
    }
    Trace tr;
    for (const Tx& x : host.tx) {
      tr.tx.push_back(std::to_string(x.kind) + "@" + std::to_string(x.at - T0) + " " + std::to_string(x.prefix[0]) +
                      " a" + std::to_string(x.attempt) + (x.flood ? " flood" : "") + " n" +
                      std::to_string(x.items.size() + x.hashes.size()));
    }
    for (const Pushed& p : host.status) tr.status.push_back(std::to_string(p.e.ts) + ":" + std::to_string((int)p.s));
    tr.confirmed = host.confirmed;
    tr.states = allStates();
    tr.idle_calls = host.idle_calls;
    return tr;
  };
  Trace reference = run(true);
  Trace cached = run(false);
  EXPECT_EQ(cached.tx, reference.tx);
  EXPECT_EQ(cached.status, reference.status);
  EXPECT_EQ(cached.confirmed, reference.confirmed);
  EXPECT_EQ(cached.states, reference.states);
  EXPECT_GE(reference.tx.size(), 20u) << "the script must exercise DM backoff, floods, queries, STATUS and deposits";
  EXPECT_TRUE(std::any_of(reference.tx.begin(), reference.tx.end(),
                          [](const std::string& s) { return s.find("flood") != std::string::npos; }));
  EXPECT_LT(cached.idle_calls * 20, reference.idle_calls) << "the cache skips the scans between due moments";
}

TEST_F(OutboxTest, IdleCacheDoesNotOutliveAMutator) {
  send("hi");
  timeout();   // RETRY, first DM at T0 + 30 + 60
  loopAt(T0 + 40);
  EXPECT_TRUE(host.tx.empty());
  ob->onHeard(ALICE, true, now);   // G15: DM at once, inside a period the cache had marked idle
  EXPECT_EQ(ob->nextDue(now), now);
  loopAt(now);
  EXPECT_EQ(host.count(Tx::DM), 1u);
}

// ---- hearing Alice (G15) ----

TEST_F(OutboxTest, G15_HearingAliceInRetrySendsDmAtMostHourly) {
  setCap();
  send("hi");
  timeout();
  runTo(T0 + 200);
  ASSERT_EQ(host.count(Tx::DM), 0u);
  ob->onHeard(ALICE, true, now);
  runToTx(now + 100, Tx::DM);
  EXPECT_EQ(host.count(Tx::DM), 1u) << "whole DM despite known cap";
  runTo(now + RDM_WAIT_ACK_MIN_S);
  ob->onHeard(ALICE, true, now);
  runTo(now + 600);
  EXPECT_EQ(host.count(Tx::DM), 1u);
}

TEST_F(OutboxTest, G15_DmWithoutCapTrailerClearsCapAndTrailerSetsIt) {
  setCap();
  ob->onHeard(ALICE, false, now);
  EXPECT_FALSE(capOf());
  ob->onHeard(ALICE, true, now);
  EXPECT_TRUE(capOf());
}

// ---- CTRL entries (G14 at Alice's side) ----

namespace {
std::vector<uint8_t> ctrlPlain(uint32_t ts, uint8_t sub, uint8_t fill) {
  std::vector<uint8_t> p(sub == CTRL_MBX_INFO ? 47 : 7, fill);
  memcpy(p.data(), &ts, 4);
  p[4] = TXT_TYPE_RDM_CTRL << 2;
  p[5] = sub;
  p[6] = CTRL_VERSION;
  return p;
}
}

TEST_F(OutboxTest, CtrlEntryIsSentWithBackoffUntilAckedWithoutAppPushes) {
  std::vector<uint8_t> p = ctrlPlain(5000, CTRL_MBX_INFO, 0x77);
  ASSERT_TRUE(ob->addCtrl(ALICE, p.data(), p.size(), now));
  EXPECT_TRUE(ob->addCtrl(ALICE, p.data(), p.size(), now)) << "same content is idempotent";
  EXPECT_EQ(ob->count(), 1);
  OutEntry e = entry();
  EXPECT_TRUE(e.flags & OF_CTRL);
  EXPECT_EQ(e.ts, 5000u);
  EXPECT_EQ(e.text_len, 42);
  EXPECT_EQ(e.text[0], CTRL_MBX_INFO);

  EXPECT_EQ(ob->nextDue(now), now);
  loopAt(now);
  ASSERT_EQ(host.count(Tx::DM), 1u);
  EXPECT_EQ(host.tx[0].attempt, 0);
  runTo(T0 + 30 + 60 + 300 + 1);
  std::vector<uint32_t> dm = times(host.tx, Tx::DM, T0);
  EXPECT_EQ(dm, (std::vector<uint32_t>{0, 90, 390})) << "G16 first retry, then DM backoff";

  uint8_t ack[4];
  crypto::ctrlAck(ack, p.data(), p.size(), host.self);
  EXPECT_TRUE(ob->onAck(ack, 4, now));
  EXPECT_EQ(ob->count(), 0);
  EXPECT_TRUE(host.status.empty());
  EXPECT_TRUE(host.confirmed.empty());
  EXPECT_TRUE(contacts->find(ALICE)->flags & CR_MBX_INFO_SENT);
}

TEST_F(OutboxTest, CtrlNewerContentReplacesPendingOne) {
  std::vector<uint8_t> info = ctrlPlain(5000, CTRL_MBX_INFO, 0x11);
  std::vector<uint8_t> revoke = ctrlPlain(5001, CTRL_MBX_REVOKE, 0);
  ASSERT_TRUE(ob->addCtrl(ALICE, info.data(), info.size(), now));
  ASSERT_TRUE(ob->addCtrl(ALICE, revoke.data(), revoke.size(), now));
  EXPECT_EQ(ob->count(), 1);
  EXPECT_EQ(entry().text[0], CTRL_MBX_REVOKE);
  uint8_t old_ack[4];
  crypto::ctrlAck(old_ack, info.data(), info.size(), host.self);
  EXPECT_FALSE(ob->onAck(old_ack, 4, now));

  std::vector<uint8_t> bad = revoke;
  bad[4] = 0;
  EXPECT_FALSE(ob->addCtrl(ALICE, bad.data(), bad.size(), now));
  EXPECT_FALSE(ob->addCtrl(ALICE, revoke.data(), 6, now));
}

TEST_F(OutboxTest, CtrlExpiresAfterTRadio) {
  std::vector<uint8_t> p = ctrlPlain(5000, CTRL_MBX_INFO, 0x77);
  ob->addCtrl(ALICE, p.data(), p.size(), now);
  runTo(T0 + RDM_T_RADIO_S - 1);
  EXPECT_EQ(ob->count(), 1);
  loopAt(T0 + RDM_T_RADIO_S);
  EXPECT_EQ(ob->count(), 0);
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

// ---- clock jump at the first trusted time (D1, G6) ----
// Until the RTC is trusted the RDM clock counts on-time only. When it then jumps to the wall clock, what was
// timestamped since boot moves along; records from earlier boots were on the trusted scale already.

TEST_F(OutboxTest, ClockJumpDoesNotAgeAFinalReachedBeforeIt) {
  host.rdm_client = false;
  uint32_t app_ack = send("hi");
  sendAck("hi", true);
  sendAckS("hi");
  ASSERT_EQ(state(), OutState::DELIVERED);
  uint32_t fin = now;
  ob->onClockJump(2 * DAY);
  now = host.now = fin + 2 * DAY;
  EXPECT_EQ(entry().final_at, fin + 2 * DAY);
  EXPECT_EQ(ob->nextDue(now), fin + 2 * DAY + RDM_FINAL_KEEP_S);
  loopAt(now);
  ASSERT_EQ(ob->count(), 1) << "the two days before the jump are not two days since the final state (G2)";
  host.rdm_client = true;
  ob->onClientConnected(true, now);
  EXPECT_EQ(lastStatus(), UserStatus::DELIVERED);
  EXPECT_EQ(host.status.back().e.app_ack, app_ack);
  EXPECT_EQ(ob->count(), 0);
}

TEST_F(OutboxTest, ClockJumpMovesCreatedAndTRadioOfEntriesMadeSinceBootAndPersistsThem) {
  host.rdm_client = false;
  send("hi");
  timeout();
  ASSERT_EQ(state(), OutState::RETRY);
  ob->onClockJump(30 * DAY);
  EXPECT_EQ(entry().created, T0 + 30 * DAY);
  EXPECT_EQ(entry().deadline, T0 + 30 * DAY + RDM_T_RADIO_S);
  boot(T0 + 30 * DAY);
  EXPECT_EQ(entry().created, T0 + 30 * DAY) << "the shifted record is on flash";
  runTo(T0 + 30 * DAY + RDM_T_RADIO_S - 1);
  EXPECT_EQ(state(), OutState::RETRY);
  loopAt(T0 + 30 * DAY + RDM_T_RADIO_S);
  EXPECT_EQ(state(), OutState::EXPIRED);
}

TEST_F(OutboxTest, ClockJumpLeavesRecordsFromEarlierBootsAlone) {
  host.rdm_client = false;
  send("hi");
  sendAck("hi", true);
  ASSERT_EQ(state(), OutState::ON_RADIO);
  boot(T0 + 100);
  uint32_t writes = io.writeCalls();
  ob->onClockJump(10 * DAY);
  EXPECT_EQ(io.writeCalls(), writes) << "nothing to shift, nothing written";
  EXPECT_EQ(entry().created, T0);
  EXPECT_EQ(entry().deadline, T0 + RDM_T_SYNC_S);
}

// leaveCopy: the deadline comes from `created` (earlier boot, trusted scale) or from `now` (this boot, on-time
// scale); only the latter moves.
TEST_F(OutboxTest, ClockJumpMovesAFallbackDeadlineOnlyWhenItCameFromNow) {
  host.rdm_client = false;
  send("hi");
  sendAck("hi", true);
  boot(T0 + 100);
  reply("hi", QueryState::UNKNOWN);
  ASSERT_EQ(state(), OutState::RETRY);
  ASSERT_EQ(entry().deadline, T0 + RDM_T_RADIO_S) << "created + T_radio, from before this boot";
  ob->onClockJump(DAY);
  EXPECT_EQ(entry().deadline, T0 + RDM_T_RADIO_S);

  boot(T0 + 100);
  send("late");
  sendAck("late", true);
  boot(T0 + RDM_T_RADIO_S - 3600);
  reply("late", QueryState::UNKNOWN);
  uint32_t idx = ob->count() - 1;
  ASSERT_EQ(state(idx), OutState::RETRY);
  ASSERT_EQ(entry(idx).deadline, now + RDM_RETRY_AFTER_LOSS_S) << "at least a day more, from now";
  ob->onClockJump(DAY);
  EXPECT_EQ(entry(idx).deadline, now + RDM_RETRY_AFTER_LOSS_S + DAY);
  EXPECT_EQ(entry(0).deadline, T0 + RDM_T_RADIO_S) << "the other entry was not touched since boot";
}

TEST_F(OutboxTest, ClockJumpMovesTheOnRadioAndCustodyDeadlines) {
  host.rdm_client = false;
  send("radio");
  sendAck("radio", true);
  ASSERT_EQ(entry(0).deadline, T0 + RDM_T_SYNC_S);
  uint32_t in_custody = toCustody("custody", 10 * DAY);
  ASSERT_EQ(entry(1).deadline, in_custody + 10 * DAY + RDM_CUSTODY_GRACE_S);
  ob->onClockJump(20 * DAY);
  EXPECT_EQ(entry(0).deadline, T0 + 20 * DAY + RDM_T_SYNC_S);
  EXPECT_EQ(entry(1).deadline, in_custody + 20 * DAY + 10 * DAY + RDM_CUSTODY_GRACE_S);
  now = host.now = in_custody + 20 * DAY;
  runTo(T0 + 20 * DAY + RDM_T_SYNC_S - 1);
  EXPECT_EQ(state(0), OutState::ON_RADIO) << "T_sync counts from the shifted ON_RADIO moment";
}

TEST_F(OutboxTest, ClockJumpMovesAReceiptWatchMadeSinceBoot) {
  send("hi");
  sendAck("hi", true);
  runTo(now + RDM_T_SYNC_S);
  ASSERT_EQ(lastStatus(), UserStatus::SYNC_EXPIRED);
  uint32_t expired_at = now;
  ob->onClockJump(100 * DAY);
  now = host.now = expired_at + 100 * DAY + RDM_WATCH_S - 1;
  boot(now);
  EXPECT_TRUE(sendAckS("hi")) << "the watch record on flash moved along with the jump";
  EXPECT_EQ(lastStatus(), UserStatus::DELIVERED);
}

TEST_F(OutboxTest, ClockJumpLeavesAReceiptWatchFromAnEarlierBootAlone) {
  send("hi");
  sendAck("hi", true);
  runTo(now + RDM_T_SYNC_S);
  ASSERT_EQ(lastStatus(), UserStatus::SYNC_EXPIRED);
  uint32_t expired_at = now;
  boot(now + 1);
  ob->onClockJump(100 * DAY);
  now = host.now = expired_at + RDM_WATCH_S;
  loopAt(now);
  EXPECT_FALSE(sendAckS("hi")) << "expired on the trusted scale it was written on";
}

TEST_F(OutboxTest, ClockJumpMovesTheCreatedTimeOfACtrlEntry) {
  std::vector<uint8_t> p = ctrlPlain(5000, CTRL_MBX_INFO, 0x77);
  ASSERT_TRUE(ob->addCtrl(ALICE, p.data(), p.size(), now));
  ob->onClockJump(DAY);
  EXPECT_EQ(entry().created, T0 + DAY);
  EXPECT_EQ(entry().deadline, T0 + DAY + RDM_T_RADIO_S);
}
