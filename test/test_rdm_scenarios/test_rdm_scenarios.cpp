// Scenario integration tests of reliable DM in the simulator: 06 par. 6.2 (S03-S14; S01 and S02 run in
// test_rdm_node), sequences in 05-sequenties.md, rules in 03-ontwerp.md par. 4, 5 and 11. Behaviour runs on B-R-A
// (plus M at R); airtime runs a second time with every pair linked, so each packet is one hop, and compares with
// 03 par. 5 or 05 within 20%.

#include "Scenario.h"

#include <helpers/rdm/RdmCodec.h>

#include <set>

using namespace sim;
using rdm::UserStatus;

namespace {

const uint64_t SEC = 1000, MIN = 60 * SEC, HOUR = 60 * MIN, DAY = 24 * HOUR;

// A text of exactly n bytes: 157 is the fork maximum with trailer, 152 the mailbox maximum (03 par. 3), the sizes
// behind the airtime figures of 03 par. 5.
std::string textOf(size_t n, const std::string& tag) {
  std::string t = tag + " ";
  while (t.size() < n) t += (char)('a' + t.size() % 26);
  return t.substr(0, n);
}

const char* layoutName(Layout l) { return l == Layout::DIRECT ? "direct" : "via repeater"; }

bool isAckOf(const Wire& w, uint32_t ack_r) { return w.effective() == Kind::ACK && ackValue(w.ack) == ack_r; }
bool isAckS(const Wire& w, const std::vector<uint8_t>& ack_s) {
  return w.effective() == Kind::ACK && w.ack.size() >= 6 && std::equal(ack_s.begin(), ack_s.end(), w.ack.begin());
}
// Firmware attempts count down from 252 + class (03 par. 1d); the app's attempts stay below 4.
bool isFirmwareDm(const Wire& w) { return w.kind == Kind::DM && w.attempt >= 4; }

bool isResponse(Kind k) {
  return k == Kind::QUERY_RESP || k == Kind::DEPOSIT_RESP || k == Kind::FETCH_RESP || k == Kind::STATUS_RESP ||
         k == Kind::REG_RESP || k == Kind::RESP_OTHER;
}
// Responses (also inside a PATH) to request `req`: same tag, from its destination back to its origin.
std::vector<Wire> responsesTo(World& w, const Wire& req) {
  return w.wires.select([&](const Wire& x) {
    return x.original() && x.origin == req.dest && x.dest == req.origin && x.ts == req.ts && isResponse(x.effective());
  });
}
bool firstResponse(World& w, const Wire& req, Wire& out) {
  auto r = responsesTo(w, req);
  if (r.empty()) return false;
  out = r[0];
  return true;
}

rdm::QueryReply queryAnswer(const Wire& resp) {
  rdm::QueryReply r[rdm::MAX_BATCH];
  uint8_t n = 0;
  if (!rdm::codec::parseQueryResp(resp.body.data(), resp.body.size(), r, n) || n < 1) {
    ADD_FAILURE() << "not a RECEIPT_QUERY answer";
    r[0].state = (rdm::QueryState)0xFF;
  }
  return r[0];
}
rdm::MbxCode depositCode(const Wire& resp) {
  rdm::MbxCode st = (rdm::MbxCode)0xFF;
  uint8_t hash[8];
  uint32_t ttl;
  if (!rdm::codec::parseDepositResp(resp.body.data(), resp.body.size(), st, hash, ttl)) ADD_FAILURE() << "bad DEPOSIT answer";
  return st;
}
struct FetchReq { uint8_t flags = 0; uint32_t store_id = 0; std::vector<rdm::Report> reports; };
FetchReq fetchReq(const Wire& req) {
  FetchReq f;
  rdm::Report r[rdm::MAX_BATCH];
  uint8_t n = 0;
  if (!rdm::codec::parseFetch(req.body.data(), req.body.size(), f.flags, f.store_id, r, n)) ADD_FAILURE() << "bad FETCH";
  f.reports.assign(r, r + n);
  return f;
}
struct FetchResp { uint8_t remaining = 0, reports_ok = 0; std::vector<uint8_t> inner; };
FetchResp fetchResp(const Wire& resp) {
  FetchResp f;
  const uint8_t* inner = nullptr;
  uint8_t len = 0;
  if (!rdm::codec::parseFetchResp(resp.body.data(), resp.body.size(), f.remaining, f.reports_ok, inner, len)) {
    ADD_FAILURE() << "bad FETCH answer";
  }
  if (inner) f.inner.assign(inner, inner + len);
  return f;
}
rdm::StatusReply statusAnswer(const Wire& resp) {
  rdm::StatusReply r[rdm::MAX_BATCH];
  uint8_t n = 0;
  if (!rdm::codec::parseStatusResp(resp.body.data(), resp.body.size(), r, n) || n < 1) {
    ADD_FAILURE() << "bad STATUS answer";
    r[0].state = (rdm::MbxState)0xFF;
  }
  return r[0];
}

// Drops the first transmission of `from` that matches, for every receiver (a lost packet).
void dropFirst(World& w, const SimNode& from, std::function<bool(const Wire&)> match) {
  auto seq = std::make_shared<int64_t>(-1);
  int idx = from.index();
  w.s.add_drop_filter([&w, seq, idx, match](const TxRecord& tx, int) {
    if (*seq >= 0) return (int64_t)tx.seq == *seq;
    if (tx.from != idx || !match(w.wires.all().back())) return false;
    *seq = (int64_t)tx.seq;
    return true;
  });
}

// Original transmissions of `from` with kind k that overlap another node's original transmission in time: at a shared
// repeater both are lost (hidden terminals, 07 par. 6).
std::string overlapping(World& w, const SimNode& from, Kind k, uint64_t t) {
  std::string out;
  for (const Wire& a : w.wires.sent(from, k, t)) {
    for (const Wire& b : w.wires.select([&](const Wire& x) {
           return x.original() && x.origin != a.origin && x.t_ms < a.t_ms + a.airtime_ms && a.t_ms < x.t_ms + x.airtime_ms;
         })) {
      out += "collides: " + w.wires.describe(a) + "  with  " + w.wires.describe(b) + "\n";
    }
  }
  return out.empty() ? "no overlapping transmissions\n" : out;
}

std::string times(const std::vector<Wire>& v, uint64_t base) {
  std::string s;
  for (const auto& x : v) s += std::to_string((long long)(x.t_ms - base) / 1000) + "s ";
  return s;
}
#define EXPECT_NEAR_MS(expected, actual, tol) EXPECT_NEAR((double)(expected), (double)(actual), (double)(tol))

// G17: a repeating interval I is I + U[0, J] with J = min(I / RDM_JITTER_DIV, RDM_JITTER_MAX_S); `slack_ms` covers
// the RDM-second rounding and the airtime between the moment the node decided and the moment we observe.
::testing::AssertionResult afterInterval(uint64_t base_ms, uint64_t actual_ms, uint32_t interval_s,
                                         uint64_t slack_ms = SEC) {
  uint32_t j = std::min<uint32_t>(interval_s / RDM_JITTER_DIV, RDM_JITTER_MAX_S);
  int64_t d = (int64_t)actual_ms - (int64_t)base_ms;
  int64_t lo = (int64_t)interval_s * 1000 - (int64_t)slack_ms, hi = (int64_t)(interval_s + j) * 1000 + (int64_t)slack_ms;
  char buf[160];
  snprintf(buf, sizeof(buf), "interval %.3f s, expected [%u, %u] s (G17)", d / 1000.0, interval_s, interval_s + j);
  if (d < lo || d > hi) return ::testing::AssertionFailure() << buf;
  return ::testing::AssertionSuccess() << buf;
}

}

// 05 scenario 1 on the airtime topology (the behaviour runs in test_rdm_node, RdmSim.S01_*): with a path known, one
// fork DM of 157 bytes, ACK_R with cap and ACK_S cost 2.3 s per hop.
TEST(RdmScenario, S01_Zendtijd) {
  WorldOptions o;
  o.layout = Layout::DIRECT;
  World w(o);
  const std::string text = textOf(157, "S01");
  const uint64_t t0 = w.s.now();
  int id = w.bobSends(text);
  ASSERT_TRUE(w.runUntil([&] { return w.bobApp().hasStatus(id, UserStatus::DELIVERED); }, 2 * MIN)) << w.trace(id);
  const uint64_t t_done = w.s.now();
  w.runFor(10 * MIN);
  EXPECT_EQ(3u, w.wires.select([&](const Wire& x) { return x.t_ms >= t0; }).size()) << "DM, ACK_R, ACK_S\n"
                                                                                  << w.wires.dump(t0);
  ::testing::AssertionResult air = airtimeWithin(w.wires.airtime([&](const Wire& x) { return x.t_ms >= t0 && x.t_ms <= t_done; }), 2.3);
  printf("[ S01 ] %s\n", air.message());
  EXPECT_TRUE(air) << w.wires.dump(t0);
}

// 05 scenario 2 on the airtime topology: Alice's phone away for the three ON_RADIO queries (1 h, 4 h and 12 h, each
// after the previous one, RdmConfig.h, with G17 jitter), then the sync: 4.7 s per hop.
TEST(RdmScenario, S02_Zendtijd) {
  WorldOptions o;
  o.layout = Layout::DIRECT;
  World w(o);
  w.aliceApp().disconnect();
  const std::string text = textOf(157, "S02");
  const uint64_t t0 = w.s.now();
  int id = w.bobSends(text);
  ASSERT_TRUE(w.runUntil([&] { return w.bobApp().hasStatus(id, UserStatus::ON_RADIO); }, 2 * MIN)) << w.trace(id);
  const uint64_t t_on = w.bobApp().firstStatusAt(id, UserStatus::ON_RADIO);
  w.runFor(17 * HOUR + 30 * MIN);

  auto q = w.wires.sent(*w.bob, Kind::QUERY, t0);
  ASSERT_EQ(3u, q.size()) << times(q, t_on);
  EXPECT_TRUE(afterInterval(t_on, q[0].t_ms, 3600));
  EXPECT_TRUE(afterInterval(q[0].t_ms, q[1].t_ms, 14400));
  EXPECT_TRUE(afterInterval(q[1].t_ms, q[2].t_ms, 43200));
  for (const auto& x : q) {
    Wire r;
    ASSERT_TRUE(firstResponse(w, x, r)) << w.wires.describe(x);
    EXPECT_EQ(rdm::QueryState::ON_RADIO, queryAnswer(r).state);
  }

  w.aliceApp().connect();
  ASSERT_TRUE(w.runUntil([&] { return w.bobApp().hasStatus(id, UserStatus::DELIVERED); }, MIN)) << w.trace(id);
  const uint64_t t_done = w.s.now();
  ::testing::AssertionResult air = airtimeWithin(w.wires.airtime([&](const Wire& x) { return x.t_ms >= t0 && x.t_ms <= t_done; }), 4.7);
  printf("[ S02 ] %s\n", air.message());
  EXPECT_TRUE(air) << w.wires.dump(t0, t_done + 1);
}

// 05 scenario 2, variant with a lost ACK_S (the base run is RdmSim.S01_*/S02_* in test_rdm_node): the next query,
// 24 h after the third (ON_RADIO schedule 1 h, 4 h, 12 h as intervals, RdmConfig.h), reads SYNCED with ACK_S.
TEST(RdmScenario, S02_AliceTelefoonWeg_AckSVerloren) {
  World w{WorldOptions()};
  w.aliceApp().disconnect();
  const std::string text = textOf(157, "S02b");
  const uint64_t t0 = w.s.now();
  int id = w.bobSends(text);
  ASSERT_TRUE(w.runUntil([&] { return w.bobApp().hasStatus(id, UserStatus::ON_RADIO); }, 2 * MIN));
  w.runFor(17 * HOUR + 30 * MIN);
  auto q = w.wires.sent(*w.bob, Kind::QUERY, t0);
  ASSERT_EQ(3u, q.size());

  auto ack_s = expectAckS(w.bobApp().sent(id).ts, text, w.bob->identity().pub_key);
  dropFirst(w, *w.alice, [&](const Wire& x) { return isAckS(x, ack_s); });
  w.aliceApp().connect();
  w.runFor(10 * MIN);
  EXPECT_EQ(1u, w.s.dropped_filter()) << "the loose ACK_S";
  EXPECT_FALSE(w.bobApp().hasStatus(id, UserStatus::DELIVERED));

  ASSERT_TRUE(w.runUntil([&] { return w.bobApp().hasStatus(id, UserStatus::DELIVERED); }, 25 * HOUR)) << w.trace(id);
  auto q4 = w.wires.sent(*w.bob, Kind::QUERY, q[2].t_ms + 1);
  ASSERT_EQ(1u, q4.size());
  EXPECT_TRUE(afterInterval(q[2].t_ms, q4[0].t_ms, 86400));
  Wire r;
  ASSERT_TRUE(firstResponse(w, q4[0], r));
  rdm::QueryReply a = queryAnswer(r);
  EXPECT_EQ(rdm::QueryState::SYNCED, a.state);
  EXPECT_TRUE(std::equal(ack_s.begin(), ack_s.end(), a.ack));
  EXPECT_LT(w.bobApp().firstStatusAt(id, UserStatus::DELIVERED) - r.t_ms, 10 * SEC);
  EXPECT_EQ(S({ UserStatus::QUEUED, UserStatus::ON_RADIO, UserStatus::DELIVERED }), w.bobApp().statuses(id));
}

// 05 scenario 3: Alice's radio off from before the send, on after 5 h. The standard app's retries land on the one
// outbox entry; the outbox probes every 30 min; the first probe after power-on reads UNKNOWN and the DM follows.
namespace {

void runS03(CompanionModel model) {
  WorldOptions o;
  o.model = model;
  World w(o);
  w.powerOff(*w.alice);
  const std::string text = textOf(120, "S03");
  const uint64_t t0 = w.s.now();
  int id = w.bobSends(text, AppRetry{3, false});
  ASSERT_TRUE(w.runUntil([&] { return w.bobApp().sent(id).failed; }, 10 * MIN));
  const auto& m = w.bobApp().sent(id);
  ASSERT_EQ(3u, m.app_acks.size());
  EXPECT_NE(m.app_acks[0], m.app_acks[1]);
  EXPECT_NE(m.app_acks[1], m.app_acks[2]);
  {
    const size_t listings = w.bobApp().outboxListings();
    w.bobApp().requestOutbox();   // CMD_RDM_LIST_OUTBOX
    ASSERT_TRUE(w.runUntil([&] { return w.bobApp().outboxListings() > listings; }, 10 * SEC));
    int entries = 0;
    for (const auto& row : w.bobApp().outbox()) {
      if (row.ts != m.ts) continue;
      entries++;
      EXPECT_EQ(m.app_acks[2], row.app_ack) << "app_ack of the latest app attempt";
      EXPECT_EQ(UserStatus::QUEUED, row.status);
    }
    EXPECT_EQ(1, entries) << "app retries land on the same entry";
  }
  auto app_dms = w.wires.sent(*w.bob, Kind::DM, t0);
  ASSERT_EQ(3u, app_dms.size()) << w.wires.dump(t0);
  for (int i = 0; i < 3; i++) EXPECT_EQ(i, app_dms[i].attempt);

  w.runFor(t0 + 5 * HOUR - w.s.now());
  const uint64_t t_up = w.s.now();
  w.powerOn(*w.alice);
  w.aliceApp().connect();
  ASSERT_TRUE(w.runUntil([&] { return w.bobApp().hasStatus(id, UserStatus::DELIVERED); }, 2 * HOUR)) << w.trace(id);

  auto probes = w.wires.sent(*w.bob, Kind::QUERY, t0, t_up);
  ASSERT_GE(probes.size(), 9u) << times(probes, t0);
  for (size_t i = 1; i < probes.size(); i++)
    EXPECT_TRUE(afterInterval(probes[i - 1].t_ms, probes[i].t_ms, RDM_SCHED_PROBE_DAY1)) << times(probes, t0);
  EXPECT_TRUE(w.wires.select([&](const Wire& x) { return isFirmwareDm(x) && x.t_ms < t_up; }).empty())
      << "cap known: probes, no DMs, in the first hours";

  auto after = w.wires.sent(*w.bob, Kind::QUERY, t_up);
  ASSERT_GE(after.size(), 1u);
  Wire r;
  ASSERT_TRUE(firstResponse(w, after[0], r)) << w.wires.dump(t_up);
  EXPECT_EQ(rdm::QueryState::UNKNOWN, queryAnswer(r).state);
  auto fw = w.wires.select([&](const Wire& x) { return x.original() && x.origin == w.bob->index() && isFirmwareDm(x) && x.t_ms > r.t_ms; });
  ASSERT_GE(fw.size(), 1u);
  EXPECT_LT(fw[0].t_ms - r.t_ms, 60 * SEC) << "DM right after UNKNOWN, over the path from the answer";
  EXPECT_EQ(rdm::FW_ATTEMPT_BASE + 0, fw[0].attempt) << "252 + class of the first app attempt";
  EXPECT_TRUE(fw[0].cap);
  EXPECT_EQ(S({ UserStatus::QUEUED, UserStatus::ON_RADIO, UserStatus::DELIVERED }), w.bobApp().statuses(id));
  EXPECT_EQ(1u, w.aliceApp().inboxCount(text));
  EXPECT_EQ(1u, w.bobApp().confirmCount(id)) << "late SEND_CONFIRMED";
}

}

TEST(RdmScenario, S03_AliceRadioUit) { runS03(CompanionModel::SIM); }
TEST(RdmScenario, S03_AliceRadioUit_CompanionNode) { runS03(CompanionModel::FIRMWARE); }

namespace {

// 05 scenario 4 up to Bob's return: ON_RADIO, Bob off, Alice syncs (her only ACK_S is lost), fill(), Bob on.
void runS04(World& w, const std::function<void()>& while_bob_off, rdm::QueryState expect_answer) {
  w.aliceApp().disconnect();
  const std::string text = textOf(140, "S04");
  int id = w.bobSends(text);
  ASSERT_TRUE(w.runUntil([&] { return w.bobApp().hasStatus(id, UserStatus::ON_RADIO); }, 2 * MIN));
  w.runFor(MIN);
  const uint64_t t_off = w.s.now();
  const size_t pushes = w.bobApp().statusLog().size();
  w.powerOff(*w.bob);

  w.aliceApp().connect();
  w.runFor(MIN);
  auto ack_s = expectAckS(w.bobApp().sent(id).ts, text, w.bob->identity().pub_key);
  auto s_acks = w.wires.select([&](const Wire& x) { return x.original() && x.origin == w.alice->index() && isAckS(x, ack_s); });
  ASSERT_EQ(1u, s_acks.size()) << "Alice sends her ACK_S while Bob is off";
  EXPECT_GT(s_acks[0].t_ms, t_off);
  while_bob_off();
  w.runFor(t_off + 2 * HOUR - w.s.now());

  const uint64_t t_boot = w.s.now();
  w.powerOn(*w.bob);
  w.runFor(5 * MIN);
  auto q = w.wires.sent(*w.bob, Kind::QUERY, t_boot);
  ASSERT_GE(q.size(), 1u) << w.wires.dump(t_boot);
  EXPECT_GE(q[0].t_ms, t_boot + 60 * SEC) << "boot kick 60-120 s after boot (G6)";
  EXPECT_LE(q[0].t_ms, t_boot + 121 * SEC);
  Wire r;
  ASSERT_TRUE(firstResponse(w, q[0], r));
  rdm::QueryReply a = queryAnswer(r);
  EXPECT_EQ(expect_answer, a.state);
  if (expect_answer == rdm::QueryState::SYNCED) EXPECT_TRUE(std::equal(ack_s.begin(), ack_s.end(), a.ack));
  EXPECT_EQ((int)rdm::OutState::DELIVERED, w.outState(w.bobCtl(), *w.alice, w.bobApp().sent(id).ts));
  EXPECT_EQ(pushes, w.bobApp().statusLog().size()) << "no 0x91 without a connected RDM client";

  const uint64_t t_conn = w.s.now();
  w.bobApp().connect();
  w.runFor(10 * SEC);
  EXPECT_EQ(S({ UserStatus::QUEUED, UserStatus::ON_RADIO, UserStatus::DELIVERED }), w.bobApp().statuses(id));
  EXPECT_GE(w.bobApp().firstStatusAt(id, UserStatus::DELIVERED), t_conn) << "unreported DELIVERED after CMD_RDM_ENABLE";
  EXPECT_EQ(1u, w.bobApp().confirmCount(id));
  EXPECT_EQ(1u, w.aliceApp().inboxCount(text));
}

}

TEST(RdmScenario, S04_BobWegVoorAckS) {
  World w{WorldOptions()};
  runS04(w, [] {}, rdm::QueryState::SYNCED);
}

// Variant: Alice's register fills up while Bob is away (synced entries evicted, watermark set): EVICTED in ON_RADIO
// also means DELIVERED. A third fork companion fills the register; Alice's flash leaves RDM few register slots.
TEST(RdmScenario, S04_BobWegVoorAckS_GevuldRegister) {
  WorldOptions o;
  o.alice_rdm_flash = 17 * 1024;
  World w(o);
  RecordDump reg = readRecords(w.alice->fs(), "/rdm/register");
  ASSERT_TRUE(reg.ok);
  ASSERT_LT(reg.slots, 64) << "register should be small for this variant";

  auto& carol = w.s.add<RdmSimCompanion>("carol");
  w.s.link(carol, *w.rep);
  w.wires.addNode(carol);
  runS04(w, [&] {
    carol.connect(true);
    carol.advert();
    w.runFor(5 * SEC);
    w.aliceCtl().advert();
    w.runFor(5 * SEC);
    carol.add_contact(*w.alice, true);
    w.aliceCtl().addContact(carol, true);
    w.runFor(SEC);
    for (int i = 0; i < reg.slots + 2; i++) {
      int c = carol.send_text(*w.alice, "fill " + std::to_string(i));
      ASSERT_GE(c, 0);
      uint32_t ack = carol.sent(c).app_ack;
      ASSERT_TRUE(w.runUntil([&] {
        for (auto st : carol.statuses_for(ack)) if (st == UserStatus::DELIVERED) return true;
        return false;
      }, 10 * MIN)) << "filler " << i << "\n" << w.wires.dump(w.s.now() - 10 * MIN);   // G16 recovers lost ACKs
    }
  }, rdm::QueryState::EVICTED);
}

namespace {

// Alice radio unreachable for Bob the whole time (her link cut), powered on and off in turns, Bob always on.
void aliceAlternates(World& w, uint64_t until_ms, const std::function<bool()>& stop) {
  bool on = false;
  while (w.s.now() < until_ms && !stop()) {
    uint64_t chunk = on ? 6 * HOUR : 18 * HOUR;
    w.runUntil(stop, std::min(chunk, until_ms - w.s.now()));
    if (on) w.powerOff(*w.alice); else w.powerOn(*w.alice);
    on = !on;
  }
}

}

// 05 scenario 5: never reachable at the same time; EXPIRED after T_radio (7 d of RDM time, Bob on throughout).
TEST(RdmScenario, S05_NooitTegelijk) {
  World w{WorldOptions()};
  w.s.unlink(*w.alice, *w.rep);
  w.powerOff(*w.alice);
  const std::string text = textOf(100, "S05");
  const uint64_t t0 = w.s.now();
  int id = w.bobSends(text, AppRetry{3, false});
  aliceAlternates(w, t0 + 8 * DAY, [&] { return w.bobApp().hasStatus(id, UserStatus::EXPIRED); });
  ASSERT_TRUE(w.bobApp().hasStatus(id, UserStatus::EXPIRED)) << w.trace(id);
  const uint64_t t_exp = w.bobApp().firstStatusAt(id, UserStatus::EXPIRED);
  EXPECT_NEAR_MS(t0 + 7 * DAY, t_exp, 2000) << "T_radio";
  EXPECT_EQ(S({ UserStatus::QUEUED, UserStatus::EXPIRED }), w.bobApp().statuses(id));
  EXPECT_EQ(0u, w.aliceApp().inboxCount(text));

  auto day1 = w.wires.sent(*w.bob, Kind::QUERY, t0, t0 + DAY);
  EXPECT_GE(day1.size(), 45u) << "48 at 90 s + k x 1800 s, fewer with G17 jitter: " << times(day1, t0);
  EXPECT_LE(day1.size(), 48u) << times(day1, t0);
  auto fw = w.wires.select([&](const Wire& x) { return x.original() && x.origin == w.bob->index() && isFirmwareDm(x); });
  ASSERT_GE(fw.size(), 1u);
  EXPECT_GE(fw[0].t_ms, t0 + DAY - 2 * MIN) << "G15: a whole DM at 24 h despite the known cap";
  EXPECT_LE(fw[0].t_ms, t0 + DAY + 5 * MIN);
  w.runFor(MIN);
  EXPECT_EQ(-1, w.outState(w.bobCtl(), *w.alice, w.bobApp().sent(id).ts)) << "reported final state frees the slot (G2)";
}

// Variant: EXPIRED while no 0x91 client is connected: the slot stays 24 h after EXPIRED (G2, D3).
TEST(RdmScenario, S05_NooitTegelijk_SlotNaVierentwintigUur) {
  World w{WorldOptions()};
  w.s.unlink(*w.alice, *w.rep);
  w.powerOff(*w.alice);
  const uint64_t t0 = w.s.now();
  int id = w.bobSends(textOf(100, "S05b"));
  const uint32_t ts = w.bobApp().sent(id).ts;
  w.runFor(SEC);
  w.bobApp().disconnect();
  w.runFor(t0 + 7 * DAY + MIN - w.s.now());
  EXPECT_EQ((int)rdm::OutState::EXPIRED, w.outState(w.bobCtl(), *w.alice, ts));
  w.runFor(t0 + 8 * DAY - 2 * MIN - w.s.now());
  EXPECT_EQ((int)rdm::OutState::EXPIRED, w.outState(w.bobCtl(), *w.alice, ts));
  w.runFor(4 * MIN);
  EXPECT_EQ(-1, w.outState(w.bobCtl(), *w.alice, ts)) << "slot free 24 h after EXPIRED";
}

// 05 scenario 6: Alice's inbox and register are recreated after ACK_R, before the app synced. The query reads
// UNKNOWN, Bob sends again, Alice shows it once.
TEST(RdmScenario, S06_AliceVerliestInbox) {
  World w{WorldOptions()};
  w.aliceApp().disconnect();
  const std::string text = textOf(140, "S06");
  int id = w.bobSends(text);
  ASSERT_TRUE(w.runUntil([&] { return w.bobApp().hasStatus(id, UserStatus::ON_RADIO); }, 2 * MIN));
  const uint64_t t_on = w.bobApp().firstStatusAt(id, UserStatus::ON_RADIO);
  w.runFor(MIN);
  w.powerOff(*w.alice);
  w.wipeAliceInbox();
  w.powerOn(*w.alice);

  auto countOnRadio = [&] {
    int n = 0;
    for (auto st : w.bobApp().statuses(id)) n += st == UserStatus::ON_RADIO;
    return n;
  };
  ASSERT_TRUE(w.runUntil([&] { return countOnRadio() == 2; }, 2 * HOUR)) << w.trace(id) << w.wires.dump(t_on);
  auto q = w.wires.sent(*w.bob, Kind::QUERY, t_on);
  ASSERT_GE(q.size(), 1u);
  EXPECT_TRUE(afterInterval(t_on, q[0].t_ms, 3600));
  Wire r;
  ASSERT_TRUE(firstResponse(w, q[0], r));
  EXPECT_EQ(rdm::QueryState::UNKNOWN, queryAnswer(r).state) << "register gone, no watermark";
  auto fw = w.wires.select([&](const Wire& x) { return x.original() && x.origin == w.bob->index() && isFirmwareDm(x) && x.t_ms > r.t_ms; });
  ASSERT_GE(fw.size(), 1u);
  EXPECT_LT(fw[0].t_ms - r.t_ms, 60 * SEC);

  w.aliceApp().connect();
  ASSERT_TRUE(w.runUntil([&] { return w.bobApp().hasStatus(id, UserStatus::DELIVERED); }, MIN)) << w.trace(id);
  EXPECT_EQ(S({ UserStatus::QUEUED, UserStatus::ON_RADIO, UserStatus::QUEUED, UserStatus::ON_RADIO, UserStatus::DELIVERED }),
            w.bobApp().statuses(id));
  EXPECT_EQ(1u, w.aliceApp().inboxCount(text));
  EXPECT_EQ(1u, w.bobApp().confirmCount(id)) << "SEND_CONFIRMED at the first ON_RADIO only";
}

// Variant G7: synced, ACK_S lost, then the register is lost: UNKNOWN, Bob sends again, Alice shows a duplicate. D2
// accepts that over a loss; the test pins it down so a change in behaviour is noticed.
TEST(RdmScenario, S06_AliceVerliestInbox_G7Duplicaat) {
  World w{WorldOptions()};
  const std::string text = textOf(140, "S06b");
  int id = w.bobSends(text);
  auto ack_s = expectAckS(w.bobApp().sent(id).ts, text, w.bob->identity().pub_key);
  dropFirst(w, *w.alice, [&](const Wire& x) { return isAckS(x, ack_s); });
  ASSERT_TRUE(w.runUntil([&] { return w.bobApp().hasStatus(id, UserStatus::ON_RADIO); }, 2 * MIN));
  w.runFor(MIN);
  ASSERT_EQ(1u, w.aliceApp().inboxCount(text));
  ASSERT_EQ(1u, w.s.dropped_filter());
  w.powerOff(*w.alice);
  w.wipeAliceInbox();
  w.powerOn(*w.alice);
  w.aliceApp().connect();
  ASSERT_TRUE(w.runUntil([&] { return w.bobApp().hasStatus(id, UserStatus::DELIVERED); }, 2 * HOUR)) << w.trace(id);
  EXPECT_EQ(2u, w.aliceApp().inboxCount(text)) << "D2: duplicate instead of loss";
  RecordProperty("G7_duplicate_shown", (int)w.aliceApp().inboxCount(text));
  printf("[ S06 G7 ] duplicate shown to Alice's app (D2): %zu copies\n", w.aliceApp().inboxCount(text));
}

namespace {

// First FETCH-request/answer pair of Alice at or after t whose request matches pred.
bool aliceFetch(World& w, uint64_t t, const std::function<bool(const Wire&, const FetchReq&)>& pred, Wire& req,
                Wire& resp) {
  for (const Wire& f : w.wires.sent(*w.alice, Kind::FETCH, t)) {
    if (!pred(f, fetchReq(f))) continue;
    req = f;
    return firstResponse(w, f, resp);
  }
  return false;
}

bool hasReport(const FetchReq& f, rdm::ReportResult r) {
  for (const auto& x : f.reports)
    if (x.result == r) return true;
  return false;
}

// Airtime of the request/answer pairs that make up one mailbox message in 03 par. 5 (DEPOSIT, FETCH with the copy,
// FETCH with a report, STATUS). Registration and empty hourly FETCHes are separate costs there and left out.
uint64_t mailboxMessageAirtime(World& w, uint64_t from, uint64_t to) {
  uint64_t sum = 0;
  for (const Wire& req : w.wires.select([&](const Wire& x) { return x.original() && x.t_ms >= from && x.t_ms <= to; })) {
    std::vector<Wire> resp = responsesTo(w, req);
    if (resp.empty()) continue;
    bool counts = false;
    if (req.kind == Kind::DEPOSIT) counts = depositCode(resp[0]) == rdm::MbxCode::OK;
    if (req.kind == Kind::STATUS) counts = true;
    if (req.kind == Kind::FETCH) counts = !fetchReq(req).reports.empty() || !fetchResp(resp[0]).inner.empty();
    if (!counts) continue;
    sum += w.wires.airtime([&](const Wire& x) {
      return memcmp(x.pkt_hash, req.pkt_hash, 8) == 0 || memcmp(x.pkt_hash, resp[0].pkt_hash, 8) == 0;
    });
  }
  return sum;
}

// 05 scenario 7. Alice's radio off at the send, Bob off once in custody, Alice on (FETCH), her app later, Bob on.
void runS07(Layout layout, bool real_mbxd, CompanionModel model = CompanionModel::SIM) {
  WorldOptions o;
  o.model = model;
  o.layout = layout;
  o.mailboxes = 1;
  o.real_mbxd = real_mbxd;
  o.ab_link = false;
  World w(o);
  SimNode& m = *w.mbx[0];
  ASSERT_TRUE(w.bobKnowsAliceMailbox(0));
  ASSERT_FALSE(w.s.linked(*w.bob, *w.alice));

  // M answers STORED only after the Pi committed: mbx.store (code 00) is there when M transmits the answer.
  int stored_answers = 0, stored_committed = 0;
  onTransmit(w.s, [&](const TxRecord& tx) {
    if (tx.from != m.index()) return;
    const Wire& x = w.wires.all().back();
    if (x.effective() != Kind::DEPOSIT_RESP || depositCode(x) != rdm::MbxCode::OK) return;
    stored_answers++;
    if (real_mbxd) {
      stored_committed += w.mbxd->countReplies("store", 0x00) >= 1;
    } else {
      rdm::MbxCode code;
      uint8_t hash[8];
      uint32_t ttl;
      rdm::codec::parseDepositResp(x.body.data(), x.body.size(), code, hash, ttl);
      MemMailboxBackend::MsgState ms;
      stored_committed += w.memBackend(0).messageState(hash, ms) && ms == MemMailboxBackend::MsgState::STORED;
    }
  });

  w.powerOff(*w.alice);
  const std::string text = textOf(152, "S07");
  const uint64_t t0 = w.s.now();
  int id = w.bobSends(text);
  ASSERT_TRUE(w.runUntil([&] { return w.bobApp().hasStatus(id, UserStatus::CUSTODY); }, HOUR)) << w.trace(id)
                                                                                           << w.wires.dump(t0);

  // Registration (G9). M can decrypt a REQ only from a key in its peer cache, and a first-time depositor is not in
  // it: M stays silent, and two unanswered DEPOSITs count as a cache miss. NOT_AUTH needs a key M already has.
  auto regs = w.wires.sent(*w.bob, Kind::REG, t0);
  ASSERT_EQ(1u, regs.size()) << w.wires.dump(t0);
  auto deps = w.wires.sent(*w.bob, Kind::DEPOSIT, t0);
  std::vector<Wire> before;
  for (const auto& d : deps) if (d.seq < regs[0].seq) before.push_back(d);
  ASSERT_GE(before.size(), 1u);
  Wire r;
  if (firstResponse(w, before.back(), r)) {
    EXPECT_EQ(rdm::MbxCode::NOT_AUTH, depositCode(r));
  } else {
    ASSERT_EQ(2u, before.size()) << "REGISTER after two unanswered DEPOSITs\n" << w.wires.dump(t0);
    EXPECT_FALSE(firstResponse(w, before[0], r));
  }
  Wire rr;
  ASSERT_TRUE(firstResponse(w, regs[0], rr));
  rdm::MbxCode rst;
  uint8_t ttl_days, quota;
  uint32_t time_m;
  ASSERT_TRUE(rdm::codec::parseRegResp(rr.body.data(), rr.body.size(), rst, ttl_days, quota, time_m));
  EXPECT_EQ(rdm::MbxCode::OK, rst);
  EXPECT_EQ(w.s.wall_epoch() / 3600, time_m / 3600) << "tijd_M is the Pi clock";
  ASSERT_GT(deps.back().seq, rr.seq);
  ASSERT_TRUE(firstResponse(w, deps.back(), r));
  EXPECT_EQ(rdm::MbxCode::OK, depositCode(r));
  EXPECT_EQ(1, stored_answers);
  EXPECT_EQ(stored_answers, stored_committed) << "STORED answered before the Pi committed";

  w.runFor(MIN);
  w.powerOff(*w.bob);
  w.runFor(10 * MIN);
  const uint64_t t_alice = w.s.now();
  w.powerOn(*w.alice);
  // K3 (03 par. 3): M lives in the hidden peer table, not in contacts[]; a mailbox contact would show in the app,
  // take a contact slot and keep M's path over reboots (then the boot FETCH below would not need the flood).
  EXPECT_FALSE(w.aliceCtl().isContact(m)) << "K3: Alice's mailbox is in her contacts (its advert was auto-added)";
  w.runFor(2 * HOUR);

  // FETCH via flood -> PATH without copy -> FETCH direct with the copy -> FETCH with report ON_RADIO
  auto fetches = w.wires.sent(*w.alice, Kind::FETCH, t_alice);
  ASSERT_GE(fetches.size(), 3u) << w.wires.dump(t_alice);
  EXPECT_LE(fetches[0].t_ms, t_alice + 121 * SEC) << "boot FETCH";
  EXPECT_TRUE(fetches[0].flood) << "no path to M after a reboot: flood, M answers with a PATH return";
  ASSERT_TRUE(firstResponse(w, fetches[0], r)) << w.wires.dump(t_alice);
  EXPECT_EQ(Kind::PATH, r.kind);
  EXPECT_GE(fetchResp(r).remaining, 1);
  EXPECT_TRUE(fetchResp(r).inner.empty());
  EXPECT_FALSE(fetches[1].flood);
  ASSERT_TRUE(firstResponse(w, fetches[1], r));
  EXPECT_FALSE(fetchResp(r).inner.empty()) << "copy in the direct FETCH answer";
  Wire freq, fresp;
  ASSERT_TRUE(aliceFetch(w, t_alice, [](const Wire&, const FetchReq& f) { return hasReport(f, rdm::ReportResult::ON_RADIO); },
                         freq, fresp));
  EXPECT_GE(fetchResp(fresp).reports_ok, 1) << "ON_RADIO report confirmed";
  EXPECT_TRUE(w.wires.select([&](const Wire& x) {
    return x.original() && x.origin == w.alice->index() && x.dest == w.bob->index() && x.t_ms >= t_alice;
  }).empty()) << "a copy from M gets no ACK to Bob";

  const uint64_t t_app = w.s.now();
  w.aliceApp().connect();
  w.runFor(30 * MIN);
  EXPECT_EQ(1u, w.aliceApp().inboxCount(text));
  auto ack_s = expectAckS(w.bobApp().sent(id).ts, text, w.bob->identity().pub_key);
  ASSERT_TRUE(aliceFetch(w, t_app, [](const Wire&, const FetchReq& f) { return hasReport(f, rdm::ReportResult::SYNCED); },
                         freq, fresp)) << w.wires.dump(t_app);
  for (const auto& rep : fetchReq(freq).reports) {
    if (rep.result == rdm::ReportResult::SYNCED) EXPECT_TRUE(std::equal(ack_s.begin(), ack_s.end(), rep.ack));
  }
  EXPECT_GE(fetchResp(fresp).reports_ok, 1) << "SYNCED report confirmed";

  w.runFor(HOUR);
  const uint64_t t_bob = w.s.now();
  w.powerOn(*w.bob);
  ASSERT_TRUE(w.runUntil([&] { return w.bobState(id, rdm::OutState::DELIVERED); }, 2 * HOUR)) << w.wires.dump(t_bob);
  auto st = w.wires.sent(*w.bob, Kind::STATUS, t_bob);
  ASSERT_GE(st.size(), 1u);
  EXPECT_GE(st[0].t_ms, t_bob + 60 * SEC) << "boot kick (G6)";
  EXPECT_LE(st[0].t_ms, t_bob + 121 * SEC);
  ASSERT_TRUE(firstResponse(w, st[0], r));
  rdm::StatusReply sa = statusAnswer(r);
  EXPECT_EQ(rdm::MbxState::DELIVERED, sa.state);
  EXPECT_TRUE(std::equal(ack_s.begin(), ack_s.end(), sa.ack)) << "Bob checks ACK_S himself";
  const uint64_t t_done = w.s.now();

  w.bobApp().connect();
  w.runFor(10 * SEC);
  EXPECT_EQ(S({ UserStatus::QUEUED, UserStatus::CUSTODY, UserStatus::DELIVERED }), w.bobApp().statuses(id));
  EXPECT_EQ(1u, w.bobApp().confirmCount(id)) << "G8: SEND_CONFIRMED at DELIVERED without ever seeing ACK_R";
  EXPECT_EQ(1u, w.aliceApp().inboxCount(text));

  if (layout == Layout::DIRECT) {
    ::testing::AssertionResult air = airtimeWithin(mailboxMessageAirtime(w, t0, t_done), 7.9);
    printf("[ S07 ] mailbox message %s\n", air.message());
    EXPECT_TRUE(air) << w.wires.dump(t0, t_done + 1);
  }
}

}

TEST(RdmScenario, S07_Mailbox) {
  for (Layout layout : { Layout::VIA_REPEATER, Layout::DIRECT }) {
    SCOPED_TRACE(layoutName(layout));
    runS07(layout, false);
  }
}

TEST(RdmScenario, S07_Mailbox_CompanionNode) { runS07(Layout::VIA_REPEATER, false, CompanionModel::FIRMWARE); }

TEST(RdmScenario, S07_MailboxEchteMbxd) {
  ASSERT_FALSE(SubprocessBackend::locateMbxd().empty()) << "mbxd.py not found (set RDM_MBXD or run from the repo root)";
  runS07(Layout::VIA_REPEATER, true);
}

// 05 scenario 8: Alice loses inbox and register after her ON_RADIO report, before the app synced. Her next FETCH
// carries a new store_id; M hands the copy out again under a new tag.
TEST(RdmScenario, S08_MailboxResync) {
  WorldOptions o;
  o.mailboxes = 1;
  World w(o);
  w.powerOff(*w.alice);
  const std::string text = textOf(152, "S08");
  int id = w.bobSends(text);
  ASSERT_TRUE(w.runUntil([&] { return w.bobApp().hasStatus(id, UserStatus::CUSTODY); }, HOUR)) << w.trace(id);
  const uint64_t t_alice = w.s.now();
  w.powerOn(*w.alice);
  ASSERT_TRUE(w.runUntil([&] { return w.bobApp().hasStatus(id, UserStatus::ON_RADIO); }, 3 * HOUR)) << w.trace(id);

  Wire first_req, first_copy, req2, copy2;
  ASSERT_TRUE(aliceFetch(w, t_alice, [&](const Wire& f, const FetchReq&) {
    Wire r;
    return firstResponse(w, f, r) && !fetchResp(r).inner.empty();
  }, first_req, first_copy));
  const uint32_t store_id1 = fetchReq(first_req).store_id;

  w.runFor(MIN);
  w.powerOff(*w.bob);
  w.powerOff(*w.alice);
  w.wipeAliceInbox();
  const uint64_t t_wipe = w.s.now();
  w.powerOn(*w.alice);
  w.runFor(HOUR);
  ASSERT_TRUE(aliceFetch(w, t_wipe, [&](const Wire& f, const FetchReq&) {
    Wire r;
    return firstResponse(w, f, r) && !fetchResp(r).inner.empty();
  }, req2, copy2)) << w.wires.dump(t_wipe);
  auto after = w.wires.sent(*w.alice, Kind::FETCH, t_wipe);
  ASSERT_GE(after.size(), 1u);
  EXPECT_NE(store_id1, fetchReq(after[0]).store_id) << "new store_id after the storage was recreated";
  EXPECT_NE(first_copy.ts, copy2.ts) << "other tag";
  EXPECT_NE(0, memcmp(first_copy.pkt_hash, copy2.pkt_hash, 8)) << "other packet hash";
  EXPECT_EQ(fetchResp(first_copy).inner, fetchResp(copy2).inner) << "same inner payload";

  w.aliceApp().connect();
  w.runFor(HOUR);
  const uint64_t t_bob = w.s.now();
  w.powerOn(*w.bob);
  ASSERT_TRUE(w.runUntil([&] { return w.bobState(id, rdm::OutState::DELIVERED); }, DAY)) << w.wires.dump(t_bob);
  w.bobApp().connect();
  w.runFor(10 * SEC);
  EXPECT_EQ(S({ UserStatus::QUEUED, UserStatus::CUSTODY, UserStatus::ON_RADIO, UserStatus::DELIVERED }),
            w.bobApp().statuses(id));
  EXPECT_EQ(1u, w.aliceApp().inboxCount(text));
}

// 05 scenario 9a: the Pi does not answer. NO_STORAGE after 3 s, Bob never in custody, re-deposit per G13, and the
// probe delivers directly once Alice is back. Bob registered earlier, so M can decrypt his DEPOSIT.
TEST(RdmScenario, S09a_MailboxOnbereikbaar) {
  WorldOptions o;
  o.mailboxes = 1;
  World w(o);
  w.registerBobAt(0);
  w.memBackend(0).setMode(MemMailboxBackend::Mode::NO_REPLY);
  w.powerOff(*w.alice);
  const std::string text = textOf(152, "S09a");
  const uint64_t t0 = w.s.now();
  int id = w.bobSends(text);
  w.runFor(2 * HOUR);

  auto deps = w.wires.sent(*w.bob, Kind::DEPOSIT, t0);
  ASSERT_GE(deps.size(), 3u) << w.wires.dump(t0);
  std::vector<uint64_t> answered;
  for (const auto& d : deps) {
    Wire r;
    ASSERT_TRUE(firstResponse(w, d, r)) << w.wires.describe(d);
    EXPECT_EQ(rdm::MbxCode::NO_STORAGE, depositCode(r));
    // the request reaches M when its last hop ends; M answers after its 3 s backend timeout, plus its 300 ms reply
    // delay and, for a flood request, the upstream rx delay before M even processes it (Dispatcher, score based)
    uint64_t arrived = 0;
    for (const auto& x : w.wires.select([&](const Wire& y) { return memcmp(y.pkt_hash, d.pkt_hash, 8) == 0; }))
      arrived = std::max(arrived, x.t_ms + x.airtime_ms);
    EXPECT_GE(r.t_ms, arrived + 3000);
    EXPECT_LE(r.t_ms, arrived + 3000 + (d.flood ? 5000 : 1500)) << w.wires.dump(arrived - 10 * SEC, r.t_ms + 1);
    answered.push_back(r.t_ms);
  }
  EXPECT_TRUE(afterInterval(answered[0], deps[1].t_ms, 600, 2 * SEC)) << "G13: 10 min";
  EXPECT_TRUE(afterInterval(answered[1], deps[2].t_ms, 1800, 2 * SEC)) << "G13: 30 min";

  w.powerOn(*w.alice);
  w.aliceApp().connect();
  ASSERT_TRUE(w.runUntil([&] { return w.bobApp().hasStatus(id, UserStatus::DELIVERED); }, 2 * HOUR)) << w.trace(id);
  EXPECT_EQ(S({ UserStatus::QUEUED, UserStatus::ON_RADIO, UserStatus::DELIVERED }), w.bobApp().statuses(id));
  EXPECT_FALSE(w.bobApp().hasStatus(id, UserStatus::CUSTODY));
  EXPECT_EQ(1u, w.aliceApp().inboxCount(text));
}

// 05 scenario 9b: M keeps the message but Alice does not get it there. The 24 h probe in CUSTODY reads UNKNOWN and
// the direct DM follows; the copy M hands out later is a DUPLICATE for Alice's register.
TEST(RdmScenario, S09b_MailboxAchterhaald) {
  WorldOptions o;
  o.mailboxes = 1;
  World w(o);
  w.s.link(*w.bob, *w.alice);
  w.powerOff(*w.alice);
  const std::string text = textOf(152, "S09b");
  int id = w.bobSends(text);
  ASSERT_TRUE(w.runUntil([&] { return w.bobApp().hasStatus(id, UserStatus::CUSTODY); }, HOUR)) << w.trace(id);
  const uint64_t t_custody = w.bobApp().firstStatusAt(id, UserStatus::CUSTODY);
  w.memBackend(0).setMode(MemMailboxBackend::Mode::WITHHOLD);
  w.powerOn(*w.alice);   // app still away: the copy that comes later finds the message unsynced
  ASSERT_TRUE(w.runUntil([&] { return w.bobApp().hasStatus(id, UserStatus::ON_RADIO); }, 26 * HOUR)) << w.trace(id);

  for (const auto& st : w.wires.sent(*w.bob, Kind::STATUS, t_custody)) {
    Wire r;
    if (firstResponse(w, st, r)) EXPECT_EQ(rdm::MbxState::STORED, statusAnswer(r).state);
  }
  auto q = w.wires.sent(*w.bob, Kind::QUERY, t_custody);
  ASSERT_GE(q.size(), 1u);
  EXPECT_TRUE(afterInterval(t_custody, q[0].t_ms, RDM_CUSTODY_PROBE_S, 2 * SEC)) << "probe to Alice in CUSTODY every 24 h";
  Wire r;
  ASSERT_TRUE(firstResponse(w, q[0], r));
  EXPECT_EQ(rdm::QueryState::UNKNOWN, queryAnswer(r).state);
  auto fw = w.wires.select([&](const Wire& x) { return x.original() && x.origin == w.bob->index() && isFirmwareDm(x) && x.t_ms > r.t_ms; });
  ASSERT_GE(fw.size(), 1u);
  EXPECT_LT(fw[0].t_ms - r.t_ms, 60 * SEC);

  const uint64_t t_normal = w.s.now();
  w.memBackend(0).setMode(MemMailboxBackend::Mode::NORMAL);
  w.runFor(2 * HOUR);
  Wire freq, fresp;
  EXPECT_TRUE(aliceFetch(w, t_normal, [](const Wire&, const FetchReq& f) { return hasReport(f, rdm::ReportResult::DUPLICATE); },
                         freq, fresp)) << w.wires.dump(t_normal);
  w.aliceApp().connect();
  ASSERT_TRUE(w.runUntil([&] { return w.bobApp().hasStatus(id, UserStatus::DELIVERED); }, HOUR)) << w.trace(id);
  EXPECT_EQ(S({ UserStatus::QUEUED, UserStatus::CUSTODY, UserStatus::ON_RADIO, UserStatus::DELIVERED }),
            w.bobApp().statuses(id));
  EXPECT_EQ(1u, w.aliceApp().inboxCount(text));
}

// 05 scenario 10: Alice on stock firmware (RDM off at runtime). Her ACK is upstream's, without cap byte; Bob stops
// at ON_RADIO_FINAL and sends nothing more for it.
TEST(RdmScenario, S10_AliceStandaard) {
  WorldOptions o;
  o.mailboxes = 1;   // Alice has a mailbox configured: as stock firmware she still sends no MBX_INFO
  o.warmup = false;
  World w(o);
  w.setAliceRdm(false);
  const std::string text = textOf(120, "S10");
  const uint64_t t0 = w.s.now();
  int id = w.bobSends(text);
  ASSERT_TRUE(w.runUntil([&] { return w.bobApp().hasStatus(id, UserStatus::ON_RADIO_FINAL); }, 2 * MIN)) << w.trace(id);
  const uint64_t t_final = w.s.now();

  auto dms = w.wires.sent(*w.bob, Kind::DM, t0);
  ASSERT_EQ(1u, dms.size());
  EXPECT_TRUE(dms[0].cap) << "Bob does not know yet: trailer on";
  auto acks = w.wires.select([&](const Wire& x) { return x.original() && x.origin == w.alice->index() && x.effective() == Kind::ACK; });
  ASSERT_EQ(1u, acks.size());
  // BaseChatMesh::onPeerDataRecv: sha256(ts|flags|text up to the NUL, pub_bob)[0:4] | byte after the NUL | random
  EXPECT_EQ(expectAckR(dms[0].ts, 0, text, w.bob->identity().pub_key), ackValue(acks[0].ack));
  ASSERT_GE(acks[0].ack.size(), 6u);
  EXPECT_EQ(dms[0].attempt, acks[0].ack[4]);
  EXPECT_FALSE(rdm::codec::ackHasCap(acks[0].ack.data(), (uint8_t)acks[0].ack.size())) << "no cap byte";
  EXPECT_EQ(S({ UserStatus::QUEUED, UserStatus::ON_RADIO_FINAL }), w.bobApp().statuses(id));
  EXPECT_EQ(1u, w.bobApp().confirmCount(id));
  EXPECT_GE(w.aliceApp().inboxCount(text), 1u);

  w.runFor(8 * DAY);
  EXPECT_TRUE(w.wires.select([&](const Wire& x) {
    return x.original() && x.origin == w.bob->index() && x.t_ms > t_final &&
           (x.kind == Kind::DM || x.kind == Kind::QUERY || x.kind == Kind::DEPOSIT || x.kind == Kind::STATUS);
  }).empty()) << "nothing after ON_RADIO_FINAL\n" << w.wires.dump(t_final);
  EXPECT_TRUE(w.wires.sent(*w.alice, Kind::CTRL).empty()) << "no MBX_INFO from stock Alice";
  EXPECT_TRUE(w.wires.select([&](const Wire& x) { return x.origin == w.alice->index() && x.kind == Kind::QUERY_RESP; }).empty());
  EXPECT_TRUE(w.wires.sent(*w.alice, Kind::ACK).size() == 1) << "no ACK_S";
}

// Variant: stock Alice off at the send, cap unknown: whole DMs on the backoff schedule; the one after power-on lands.
TEST(RdmScenario, S10_AliceStandaard_Backoff) {
  WorldOptions o;
  o.warmup = false;
  World w(o);
  // Bob gets a path from an earlier DM to the stock Alice (cap stays unknown). Without a path every attempt is a
  // flood, and at most one flood per contact per 4 h (03 par. 5) goes out: the backoff steps in between are dropped.
  w.setAliceRdm(false);
  int hello = w.bobSends("hello stock alice");
  ASSERT_TRUE(w.runUntil([&] { return w.bobApp().hasStatus(hello, UserStatus::ON_RADIO_FINAL); }, 2 * MIN));
  w.runFor(MIN);
  ASSERT_TRUE(w.bobCtl().hasPathTo(*w.alice));
  w.powerOff(*w.alice);
  const std::string text = textOf(120, "S10b");
  const uint64_t t0 = w.s.now();
  int id = w.bobSends(text);
  w.runFor(2 * HOUR);
  w.powerOn(*w.alice);
  w.setAliceRdm(false);
  w.aliceApp().connect();
  ASSERT_TRUE(w.runUntil([&] { return w.bobApp().hasStatus(id, UserStatus::ON_RADIO_FINAL); }, 6 * HOUR)) << w.trace(id);

  auto fw = w.wires.select([&](const Wire& x) { return x.original() && x.origin == w.bob->index() && isFirmwareDm(x) && x.t_ms >= t0; });
  ASSERT_GE(fw.size(), 4u) << times(fw, t0) << "\n" << w.wires.dump(t0);
  EXPECT_TRUE(w.wires.sent(*w.bob, Kind::QUERY, t0).empty()) << "cap unknown: no probes";
  const uint32_t sched[] = RDM_SCHED_DM_BACKOFF;
  for (size_t i = 1; i < fw.size() && i <= 4; i++)
    EXPECT_TRUE(afterInterval(fw[i - 1].t_ms, fw[i].t_ms, sched[i - 1], 2 * SEC)) << "gap " << i << ": " << times(fw, t0);
  EXPECT_EQ(S({ UserStatus::QUEUED, UserStatus::ON_RADIO_FINAL }), w.bobApp().statuses(id));
}

// G15: Alice ran the fork (cap known), then stock firmware. Bob's probes go unanswered; within 24 h he sends a whole
// DM anyway, and the ACK without cap byte clears the cap.
TEST(RdmScenario, S11_CapIngetrokken) {
  World w{WorldOptions()};
  ContactRdmView v;
  ASSERT_TRUE(readContactRdm(w.bob->fs(), w.alice->identity().pub_key, v));
  ASSERT_TRUE(v.flags & rdm::CR_CAP) << "cap known after the warm-up";
  w.powerOff(*w.alice);
  const std::string text = textOf(120, "S11");
  const uint64_t t0 = w.s.now();
  int id = w.bobSends(text);
  w.runFor(6 * HOUR);
  const uint64_t t_up = w.s.now();
  w.powerOn(*w.alice);
  w.setAliceRdm(false);
  w.aliceApp().connect();
  ASSERT_TRUE(w.runUntil([&] { return w.bobApp().hasStatus(id, UserStatus::ON_RADIO_FINAL); }, DAY)) << w.trace(id);

  EXPECT_GE(w.wires.sent(*w.bob, Kind::QUERY, t_up).size(), 1u) << "probes while the cap is known";
  EXPECT_TRUE(w.wires.select([&](const Wire& x) { return x.origin == w.alice->index() && x.kind == Kind::QUERY_RESP; }).empty())
      << "stock firmware does not answer RECEIPT_QUERY";
  auto fw = w.wires.select([&](const Wire& x) { return x.original() && x.origin == w.bob->index() && isFirmwareDm(x) && x.t_ms > t_up; });
  ASSERT_GE(fw.size(), 1u);
  EXPECT_LE(fw[0].t_ms, t0 + DAY + 5 * MIN) << "whole DM within 24 h despite the cap (G15)";
  EXPECT_EQ(S({ UserStatus::QUEUED, UserStatus::ON_RADIO_FINAL }), w.bobApp().statuses(id));
  ASSERT_TRUE(readContactRdm(w.bob->fs(), w.alice->identity().pub_key, v));
  EXPECT_FALSE(v.flags & rdm::CR_CAP) << "cap cleared after an ACK without cap byte";
}

// G14: Alice moves from M1 to M2 while Bob has the message in custody at M1. MBX_INFO repeats until Bob ACKs, Bob
// deposits at M2, the copy at M1 is never shown.
TEST(RdmScenario, S12_MailboxWissel) {
  WorldOptions o;
  o.mailboxes = 2;
  World w(o);
  ASSERT_TRUE(w.bobKnowsAliceMailbox(0));
  w.s.unlink(*w.alice, *w.rep);
  const std::string text = textOf(152, "S12");
  int id = w.bobSends(text);
  ASSERT_TRUE(w.runUntil([&] { return w.bobApp().hasStatus(id, UserStatus::CUSTODY); }, HOUR)) << w.trace(id);
  auto dep1 = w.wires.select([&](const Wire& x) { return x.original() && x.kind == Kind::DEPOSIT && x.dest == w.mbx[0]->index(); });
  ASSERT_GE(dep1.size(), 1u);

  const uint64_t t_switch = w.s.now();
  w.aliceApp().setMailbox(w.mbxPub(1), w.k_owner[1]);
  ASSERT_TRUE(w.runUntil([&] { return w.aliceApp().mailboxResult() == 1; }, 10 * SEC));
  dropFirst(w, *w.alice, [](const Wire& x) { return x.kind == Kind::CTRL; });
  w.s.link(*w.alice, *w.rep);
  ASSERT_TRUE(w.runUntil([&] { return w.bobKnowsAliceMailbox(1); }, DAY)) << w.wires.dump(t_switch);
  const uint64_t t_known = w.s.now();
  // out of Bob's reach again once his ACK is in, or he would deliver directly (I4) instead of moving the custody
  w.runFor(5 * SEC);
  w.s.unlink(*w.alice, *w.rep);
  auto custodies = [&] {
    int n = 0;
    for (auto st : w.bobApp().statuses(id)) n += st == UserStatus::CUSTODY;
    return n;
  };
  ASSERT_TRUE(w.runUntil([&] { return custodies() == 2; }, DAY)) << w.trace(id) << w.wires.dump(t_known);
  auto ctrl = w.wires.sent(*w.alice, Kind::CTRL, t_switch);
  EXPECT_GE(ctrl.size(), 2u) << "MBX_INFO repeated after the lost first one";
  // Alice fetches from M2 before her app syncs, so Bob sees ON_RADIO on the way (G8 would allow skipping it)
  w.aliceApp().disconnect();
  w.s.link(*w.alice, *w.rep);
  ASSERT_TRUE(w.runUntil([&] { return w.bobApp().hasStatus(id, UserStatus::ON_RADIO); }, DAY)) << w.trace(id);
  const uint64_t t_onr = w.s.now();
  w.aliceApp().connect();
  ASSERT_TRUE(w.runUntil([&] { return w.bobApp().hasStatus(id, UserStatus::DELIVERED); }, 3 * DAY)) << w.trace(id)
      << overlapping(w, *w.bob, Kind::STATUS, t_onr);
  const uint64_t t_delivered = w.s.now();
  w.runFor(12 * HOUR);
  EXPECT_EQ(ctrl.size(), w.wires.sent(*w.alice, Kind::CTRL, t_switch).size()) << "no MBX_INFO after Bob's ACK";

  // G17 (a): Alice's hourly FETCH drifts by U[0, 60 s] per step, so no other periodic timer stays in phase with it.
  // Without it Bob's STATUS and her FETCH met at R in the same millisecond, again and again (hidden terminals).
  // A pair with an advert of her mailbox in between is skipped: the advert trigger (03 par. 4) restarts the interval.
  auto polls = w.wires.select([&](const Wire& x) {
    return x.original() && x.origin == w.alice->index() && x.kind == Kind::FETCH && x.t_ms > t_delivered + HOUR &&
           fetchReq(x).reports.empty();
  });
  auto m2_adverts = w.wires.select([&](const Wire& x) {
    return x.original() && x.origin == w.mbx[1]->index() && x.kind == Kind::ADVERT && x.t_ms > t_delivered;
  });
  auto advertBetween = [&](uint64_t a, uint64_t b) {
    for (const auto& ad : m2_adverts) if (ad.t_ms > a && ad.t_ms < b) return true;
    return false;
  };
  ASSERT_GE(polls.size(), 6u);
  std::set<uint64_t> distinct;
  size_t pairs = 0;
  for (size_t i = 1; i < polls.size(); i++) {
    if (advertBetween(polls[i - 1].t_ms, polls[i].t_ms)) continue;
    pairs++;
    EXPECT_TRUE(afterInterval(polls[i - 1].t_ms, polls[i].t_ms, RDM_FETCH_INTERVAL_S)) << times(polls, t_delivered);
    distinct.insert(polls[i].t_ms - polls[i - 1].t_ms);
  }
  ASSERT_GE(pairs, 4u);
  EXPECT_GT(distinct.size(), 1u) << "G17: the FETCH interval never varies\n" << times(polls, t_delivered);
  auto dep2 = w.wires.select([&](const Wire& x) {
    return x.original() && x.kind == Kind::DEPOSIT && x.dest == w.mbx[1]->index() && x.t_ms >= t_known;
  });
  ASSERT_GE(dep2.size(), 1u) << "deposit at the new mailbox";
  EXPECT_EQ(S({ UserStatus::QUEUED, UserStatus::CUSTODY, UserStatus::QUEUED, UserStatus::CUSTODY, UserStatus::ON_RADIO,
                UserStatus::DELIVERED }),
            w.bobApp().statuses(id)) << "G1: DEPOSITING at M2 reads QUEUED\n" << w.wires.dump(t_switch);
  EXPECT_EQ(1u, w.aliceApp().inboxCount(text));
  EXPECT_EQ(1u, w.memBackend(0).messageCount());
  EXPECT_TRUE(w.wires.select([&](const Wire& x) {
    if (x.origin != w.mbx[0]->index() || x.effective() != Kind::FETCH_RESP) return false;
    return !fetchResp(x).inner.empty();
  }).empty()) << "the copy at M1 never went out";
}

// Beslissing 6, I8: the standard app never sends CMD_RDM_ENABLE and gets no 0x91, only SEND_CONFIRMED at ON_RADIO,
// also when it connects only after ON_RADIO (G5).
namespace {

void runS13(CompanionModel model) {
  WorldOptions o;
  o.model = model;
  o.bob_rdm_client = false;
  World w(o);
  const std::string text = textOf(120, "S13");
  int id = w.bobSends(text);
  ASSERT_TRUE(w.runUntil([&] { return w.bobApp().confirmCount(id) >= 1; }, 2 * MIN));
  const uint64_t t_conf = w.bobApp().sent(id).confirmed_at;
  auto acks = w.wires.select([&](const Wire& x) {
    return x.origin == w.alice->index() && isAckOf(x, w.bobApp().sent(id).app_acks[0]);
  });
  ASSERT_GE(acks.size(), 1u);
  EXPECT_LT(t_conf - (acks.back().t_ms + acks.back().airtime_ms), 2 * SEC) << "SEND_CONFIRMED at ACK_R";
  w.runFor(10 * MIN);
  EXPECT_TRUE(w.bobState(id, rdm::OutState::DELIVERED));
  EXPECT_EQ(1u, w.bobApp().confirmCount(id));

  int id2 = w.bobSends(textOf(120, "S13b"));
  w.bobApp().disconnect();
  ASSERT_TRUE(w.runUntil([&] { return w.outState(w.bobCtl(), *w.alice, w.bobApp().sent(id2).ts) >= (int)rdm::OutState::ON_RADIO; }, 2 * MIN));
  EXPECT_EQ(0u, w.bobApp().confirmCount(id2));
  w.bobApp().connect();
  w.runFor(10 * SEC);
  EXPECT_EQ(1u, w.bobApp().confirmCount(id2)) << "G5: confirm_pending on the next connection";
  w.bobApp().disconnect();
  w.bobApp().connect();
  w.runFor(10 * SEC);
  EXPECT_EQ(1u, w.bobApp().confirmCount(id2)) << "once";
  EXPECT_TRUE(w.bobApp().statusLog().empty()) << "no 0x91 to an app without CMD_RDM_ENABLE";
}

}

TEST(RdmScenario, S13_StandaardApp) { runS13(CompanionModel::SIM); }
TEST(RdmScenario, S13_StandaardApp_CompanionNode) { runS13(CompanionModel::FIRMWARE); }

namespace {

// Bob's WAIT_ACK timeout for the DM `dm` (G3): max(2 x est_timeout, 30 s), est_timeout as BaseChatMesh computes it.
uint64_t waitAckEnd(const Wire& dm) {
  uint64_t est = dm.flood ? 500 + 16 * (uint64_t)dm.airtime_ms
                          : 500 + ((uint64_t)dm.airtime_ms * 6 + 250) * (dm.hops + 1);
  return dm.t_ms + std::max<uint64_t>(2 * est, 30 * SEC);
}

}

// G16, 07 par. 6: the DM arrives, Alice's ACK_R is lost. The first RETRY action comes 60-120 s after the timeout: with
// a known cap a probe that reads ON_RADIO, without one a DM that Alice's register catches as a duplicate.
TEST(RdmScenario, S14_HiddenTerminalVerlorenAck) {
  for (bool cap : { true, false }) {
    SCOPED_TRACE(cap ? "cap known" : "cap unknown");
    WorldOptions o;
    o.warmup = cap;
    World w(o);
    w.aliceApp().disconnect();
    const std::string text = textOf(120, "S14");
    int id = w.bobSends(text);
    const uint32_t ack_r = w.bobApp().sent(id).app_acks[0];
    dropFirst(w, *w.alice, [&](const Wire& x) { return isAckOf(x, ack_r); });
    const uint64_t t0 = w.s.now();
    ASSERT_TRUE(w.runUntil([&] { return w.bobApp().hasStatus(id, UserStatus::ON_RADIO); }, 10 * MIN)) << w.trace(id)
                                                                                                    << w.wires.dump(t0);
    EXPECT_EQ(1u, w.s.dropped_filter());
    auto dms = w.wires.sent(*w.bob, Kind::DM, t0);
    ASSERT_GE(dms.size(), 1u);
    const uint64_t timeout = waitAckEnd(dms[0]);
    if (cap) {
      auto q = w.wires.sent(*w.bob, Kind::QUERY, t0);
      ASSERT_EQ(1u, q.size()) << w.wires.dump(t0);
      EXPECT_GE(q[0].t_ms + 1000, timeout + 60 * SEC) << "first RETRY action 60-120 s after the timeout";
      EXPECT_LE(q[0].t_ms, timeout + 121 * SEC);
      Wire r;
      ASSERT_TRUE(firstResponse(w, q[0], r));
      rdm::QueryReply a = queryAnswer(r);
      EXPECT_EQ(rdm::QueryState::ON_RADIO, a.state);
      EXPECT_EQ(ack_r, ackValue(std::vector<uint8_t>(a.ack, a.ack + 4)));
      EXPECT_EQ(1u, dms.size()) << "no second DM";
    } else {
      ASSERT_EQ(2u, dms.size()) << w.wires.dump(t0);
      EXPECT_GE(dms[1].t_ms + 1000, timeout + 60 * SEC) << "first RETRY action 60-120 s after the timeout";
      EXPECT_LE(dms[1].t_ms, timeout + 121 * SEC);
      EXPECT_TRUE(w.wires.sent(*w.bob, Kind::QUERY, t0).empty());
    }
    w.aliceApp().connect();
    ASSERT_TRUE(w.runUntil([&] { return w.bobApp().hasStatus(id, UserStatus::DELIVERED); }, 2 * MIN)) << w.trace(id);
    EXPECT_EQ(S({ UserStatus::QUEUED, UserStatus::ON_RADIO, UserStatus::DELIVERED }), w.bobApp().statuses(id));
    EXPECT_EQ(1u, w.aliceApp().inboxCount(text));
    EXPECT_EQ(1u, w.bobApp().confirmCount(id));
  }
}
