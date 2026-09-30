// S04 variants around the RDM clock (03 par. 11 G6, default D1): Bob's radio comes back without an app, so his
// RDM clock counts on-time only until the phone sets the time. What the outbox timestamped in that stretch must
// not age by the jump to the wall clock: the DELIVERED his boot query learned is still there for the 0x91 replay.

#include "Scenario.h"

#include <helpers/rdm/RdmCodec.h>

using namespace sim;
using rdm::UserStatus;

namespace {

const uint64_t SEC = 1000, MIN = 60 * SEC, HOUR = 60 * MIN, DAY = 24 * HOUR;

bool isAckS(const Wire& w, const std::vector<uint8_t>& ack_s) {
  return w.effective() == Kind::ACK && w.ack.size() >= 6 && std::equal(ack_s.begin(), ack_s.end(), w.ack.begin());
}

bool queryAnswerSynced(World& w, const Wire& req, const std::vector<uint8_t>& ack_s) {
  auto r = w.wires.select([&](const Wire& x) {
    return x.original() && x.origin == req.dest && x.dest == req.origin && x.ts == req.ts && x.effective() == Kind::QUERY_RESP;
  });
  if (r.empty()) return false;
  rdm::QueryReply a[rdm::MAX_BATCH];
  uint8_t n = 0;
  if (!rdm::codec::parseQueryResp(r[0].body.data(), r[0].body.size(), a, n) || n < 1) return false;
  return a[0].state == rdm::QueryState::SYNCED && std::equal(ack_s.begin(), ack_s.end(), a[0].ack);
}

// 05 scenario 4 with Bob away for `off_ms`: ON_RADIO, Bob off, Alice syncs (her only ACK_S is lost), Bob on
// without an app, boot query learns SYNCED, then the app connects (CMD_SET_DEVICE_TIME, CMD_RDM_ENABLE).
void runClockJump(World& w, uint64_t off_ms) {
  w.aliceApp().disconnect();
  const std::string text = "S04 clock jump";
  int id = w.bobSends(text);
  ASSERT_TRUE(w.runUntil([&] { return w.bobApp().hasStatus(id, UserStatus::ON_RADIO); }, 2 * MIN)) << w.wires.dump();
  w.runFor(MIN);
  const uint64_t t_off = w.s.now();
  const size_t pushes = w.bobApp().statusLog().size();
  w.powerOff(*w.bob);

  w.aliceApp().connect();
  w.runFor(MIN);
  auto ack_s = expectAckS(w.bobApp().sent(id).ts, text, w.bob->identity().pub_key);
  auto s_acks = w.wires.select([&](const Wire& x) { return x.original() && x.origin == w.alice->index() && isAckS(x, ack_s); });
  ASSERT_EQ(1u, s_acks.size()) << "Alice sends her ACK_S while Bob is off";
  w.runFor(t_off + off_ms - w.s.now());

  const uint64_t t_boot = w.s.now();
  w.powerOn(*w.bob);
  w.runFor(5 * MIN);
  auto q = w.wires.sent(*w.bob, Kind::QUERY, t_boot);
  ASSERT_GE(q.size(), 1u) << w.wires.dump(t_boot);
  ASSERT_TRUE(queryAnswerSynced(w, q[0], ack_s)) << w.wires.dump(t_boot);
  ASSERT_EQ((int)rdm::OutState::DELIVERED, w.outState(w.bobCtl(), *w.alice, w.bobApp().sent(id).ts));
  ASSERT_EQ(pushes, w.bobApp().statusLog().size()) << "no 0x91 without a connected RDM client";

  const uint64_t t_conn = w.s.now();
  w.bobApp().connect();
  w.runFor(10 * SEC);
  EXPECT_EQ(S({ UserStatus::QUEUED, UserStatus::ON_RADIO, UserStatus::DELIVERED }), w.bobApp().statuses(id));
  EXPECT_GE(w.bobApp().firstStatusAt(id, UserStatus::DELIVERED), t_conn) << "unreported DELIVERED after CMD_RDM_ENABLE";
  EXPECT_EQ(1u, w.bobApp().confirmCount(id));
  EXPECT_EQ(1u, w.aliceApp().inboxCount(text));
}

}

// D29: Bob was on for less than RDM_CLOCK_PERSIST_S before the power loss, so /rdm/meta still held rdm_time 0 and
// his clock restarted from 0. Two hours away is then a jump of the whole epoch at the reconnect.
TEST(RdmScenario, S04_KlokSprong_BobKortAanVoorPowerOff) {
  WorldOptions o;
  o.model = CompanionModel::FIRMWARE;
  o.warmup = false;   // the warm-up alone would keep Bob on past the first clock persist
  World w(o);
  runClockJump(w, 2 * HOUR);
}

// Bob's clock was persisted, but the two days he was off come before the DELIVERED his boot query learned: they
// are not two days since it (G2, D3) when the clock catches up at the reconnect.
TEST(RdmScenario, S04_KlokSprong_BobTweeDagenWeg) {
  WorldOptions o;
  o.model = CompanionModel::FIRMWARE;
  World w(o);
  runClockJump(w, 2 * DAY);
}
