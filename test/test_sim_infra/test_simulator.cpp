// Properties of the simulator itself: determinism, reboot semantics of RTC and flash, link model, FS faults.

#include <gtest/gtest.h>

#include <FS.h>

#include "ChatNode.h"

using namespace sim;
using Status = ChatNode::Status;

namespace {

std::vector<TxRecord> runScenario(uint64_t seed) {
  Simulator s(seed);
  auto& a = s.add<ChatNode>("a");
  auto& r = s.add<RepeaterNode>("r");
  auto& b = s.add<ChatNode>("b");
  s.link(a, r);
  s.link(r, b);
  a.advert();
  s.run_for(5000);
  b.advert();
  s.run_for(5000);
  b.send_text(a, "determinism");
  s.run_for(20000);
  return s.tx_log();
}

bool sameLog(const std::vector<TxRecord>& x, const std::vector<TxRecord>& y) {
  if (x.size() != y.size()) return false;
  for (size_t i = 0; i < x.size(); i++) {
    if (x[i].t_ms != y[i].t_ms || x[i].from != y[i].from || x[i].raw != y[i].raw) return false;
  }
  return true;
}

}

TEST(Simulator, SameSeedGivesIdenticalRun) {
  auto run1 = runScenario(99);
  auto run2 = runScenario(99);
  ASSERT_FALSE(run1.empty());
  EXPECT_TRUE(sameLog(run1, run2));
  EXPECT_FALSE(sameLog(run1, runScenario(100))) << "another seed gives other identities, so other bytes";
}

TEST(Simulator, AirtimeMatchesLoRaFormulaForMeshCoreNlSettings) {
  Simulator s(1);  // default RadioConfig: SF8, BW 62.5 kHz, CR 4/8, preamble 32
  EXPECT_EQ(s.airtime_ms(38), 509u);
  EXPECT_EQ(s.airtime_ms(8), 280u);
  EXPECT_GT(s.airtime_ms(200), s.airtime_ms(100));
}

TEST(Simulator, RtcWithoutBatteryRestartsIn2024AndBatteryKeepsTime) {
  Simulator s(1);
  auto& n = s.add<ChatNode>("n");
  EXPECT_EQ(n.rtc().getCurrentTime(), s.wall_epoch()) << "first boot: time set as if by the app";

  s.run_for(60000);
  n.reboot();
  EXPECT_EQ(n.rtc().getCurrentTime(), RTC_RESET_EPOCH);
  n.sync_clock();
  EXPECT_EQ(n.rtc().getCurrentTime(), s.wall_epoch());

  n.set_rtc_battery(true);
  n.power_off();
  s.run_for(30000);
  n.power_on();
  EXPECT_EQ(n.rtc().getCurrentTime(), s.wall_epoch());
  EXPECT_EQ(n.bootCount(), 3u);
  EXPECT_EQ(n.uptimeMs(), 0u);
}

TEST(Simulator, FlashSurvivesRebootUnlessWiped) {
  Simulator s(5);
  auto& a = s.add<ChatNode>("a");
  auto& b = s.add<ChatNode>("b");
  s.link(a, b);
  a.add_contact(b);
  b.add_contact(a);
  uint8_t before[PUB_KEY_SIZE];
  memcpy(before, a.identity().pub_key, PUB_KEY_SIZE);

  a.reboot();
  EXPECT_EQ(memcmp(before, a.identity().pub_key, PUB_KEY_SIZE), 0);
  EXPECT_TRUE(a.knows(b));
  int id = b.send_text(a, "still friends");
  ASSERT_TRUE(s.run_until([&] { return b.sent(id).status == Status::DELIVERED; }, 30000));

  a.reboot(true);
  EXPECT_NE(memcmp(before, a.identity().pub_key, PUB_KEY_SIZE), 0) << "factory reset makes a new identity";
  EXPECT_FALSE(a.knows(b));
}

TEST(Simulator, LossyAndOneWayLinks) {
  Simulator s(8);
  auto& a = s.add<ChatNode>("a");
  auto& b = s.add<ChatNode>("b");
  s.link_oneway(a, b);
  a.advert();
  b.advert();
  s.run_for(10000);
  EXPECT_TRUE(b.knows(a));
  EXPECT_FALSE(a.knows(b)) << "b cannot reach a";

  s.link(a, b, 8.0f, 1.0f);
  const uint64_t lost_before = s.dropped_loss();
  a.advert();
  s.run_for(5000);
  EXPECT_EQ(s.dropped_loss(), lost_before + 1);
}

TEST(Simulator, ReceiverPoweredOffMidFrameLosesIt) {
  Simulator s(9);
  auto& a = s.add<ChatNode>("a");
  auto& b = s.add<ChatNode>("b");
  s.link(a, b);
  a.advert();
  s.run_for(100);  // frame is on air (ADVERT takes ~1.1 s)
  b.power_off();
  b.power_on();
  s.run_for(5000);
  EXPECT_FALSE(b.knows(a));
  EXPECT_EQ(s.dropped_off(), 1u);
}

TEST(Simulator, FlashFaultInjectionTearsWritesAndCapacityIsEnforced) {
  SimFS store;
  fs::BoundFS flash(store);
  File f = flash.open("/rec", "w");
  store.fail_writes_after(3);
  EXPECT_EQ(f.write((const uint8_t*)"abcdef", 6), 3u);
  EXPECT_EQ(f.write((const uint8_t*)"gh", 2), 0u);
  f.close();
  EXPECT_EQ(store.files["/rec"].size(), 3u);

  store.write_budget = -1;
  store.capacity_bytes = 10;
  File g = flash.open("/big", "w");
  EXPECT_EQ(g.write((const uint8_t*)"0123456789", 10), 7u);  // 3 bytes already used by /rec
  EXPECT_EQ(store.used_bytes(), 10u);

  File h = flash.open("/rec", "r+");
  ASSERT_TRUE((bool)h);
  h.seek(1);
  EXPECT_EQ(h.write((const uint8_t*)"X", 1), 1u);  // in-place update needs no extra space
  EXPECT_EQ(std::string(store.files["/rec"].begin(), store.files["/rec"].end()), "aXc");
}

namespace {

std::vector<TxRecord> dmHourApart(bool fast_forward, uint64_t* final_now) {
  Simulator s(21);
  if (fast_forward) s.set_fast_forward(1000);
  auto& a = s.add<ChatNode>("a");
  auto& b = s.add<ChatNode>("b");
  s.link(a, b);
  a.advert();
  s.run_for(5000);
  b.advert();
  s.run_for(5000);
  b.send_text(a, "one");
  s.run_for(3600 * 1000);
  b.send_text(a, "two");
  s.run_for(60000);
  EXPECT_EQ(a.inbox().size(), 2u);
  *final_now = s.now();
  return s.tx_log();
}

}

TEST(Simulator, FastForwardGivesTheSameRunAsMillisecondTicks) {
  uint64_t end_slow = 0, end_fast = 0;
  auto slow = dmHourApart(false, &end_slow);
  auto fast = dmHourApart(true, &end_fast);
  EXPECT_EQ(end_slow, end_fast);
  EXPECT_TRUE(sameLog(slow, fast));
}

TEST(Simulator, FastForwardCoversDaysAndLandsOnWakeups) {
  Simulator s(22);
  s.set_fast_forward(1000);
  auto& a = s.add<ChatNode>("a");
  auto& b = s.add<ChatNode>("b");
  s.link(a, b);
  a.add_contact(b);
  b.add_contact(a);
  a.set_rtc_battery(true);

  s.run_for(8ull * 86400 * 1000);
  EXPECT_EQ(s.now(), 8ull * 86400 * 1000);
  EXPECT_EQ(a.rtc().getCurrentTime(), s.wall_epoch());

  b.power_off();
  int id = a.send_text(b, "into the void");
  const uint64_t deadline = s.now() + a.sent(id).est_timeout_ms;
  ASSERT_TRUE(s.run_until([&] { return a.sent(id).status == Status::TIMED_OUT; }, 60000));
  EXPECT_LE(s.now(), deadline + 1) << "the timeout wakeup is not overshot by a coarse step";
}

TEST(Simulator, DropNextLosesExactlyOneAck) {
  Simulator s(23);
  auto& a = s.add<ChatNode>("a");
  auto& b = s.add<ChatNode>("b");
  s.link(a, b);
  a.add_contact(b);
  b.add_contact(a);

  s.drop_next(PAYLOAD_TYPE_PATH, a.index());  // first DM is flood, so its ACK rides in a PATH packet
  int m1 = b.send_text(a, "ack lost");
  s.run_for(30000);
  EXPECT_EQ(a.inbox_count("ack lost"), 1u);
  EXPECT_EQ(b.sent(m1).status, Status::TIMED_OUT);
  EXPECT_EQ(s.dropped_filter(), 1u);

  int m2 = b.send_text(a, "ack kept");
  ASSERT_TRUE(s.run_until([&] { return b.sent(m2).status == Status::DELIVERED; }, 30000));
  EXPECT_EQ(s.tx_log().back().airtime_ms, s.airtime_ms((int)s.tx_log().back().raw.size()));
}

// H1: only a time set after boot counts as synced; a firmware bootstrap does not.
TEST(Simulator, RtcSyncFlagFollowsSetsAfterBoot) {
  Simulator s(30);
  auto& n = s.add<ChatNode>("n");
  EXPECT_FALSE(n.rtc().setSinceBoot());
  EXPECT_FALSE(n.has_rtc_battery());
  n.sync_clock();
  EXPECT_TRUE(n.rtc().setSinceBoot());
  n.reboot();
  EXPECT_FALSE(n.rtc().setSinceBoot());
  n.rtc().setCurrentTime(RTC_RESET_EPOCH + 1000);   // e.g. bootstrapRTCfromContacts
  n.rtc().markBootstrapped();
  EXPECT_FALSE(n.rtc().setSinceBoot());
  n.set_rtc_battery(true);
  EXPECT_TRUE(n.has_rtc_battery());
}

namespace {

// Writes through fs::File from its own loop, like firmware storing a record, then optionally transmits.
class WriterNode : public ChatNode {
public:
  using ChatNode::ChatNode;
  int writes_pending = 0;
  bool advert_after = false;

protected:
  void onLoop() override {
    ChatNode::onLoop();
    while (writes_pending > 0) {
      fs::BoundFS flash(fs());
      File f = flash.open("/log", "a");
      const uint8_t rec[8] = {1, 2, 3, 4, 5, 6, 7, 8};
      f.write(rec, sizeof(rec));
      writes_pending--;
    }
    if (advert_after) {
      advert_after = false;
      advert();
    }
  }
};

}

// H2: a power cut mid-write leaves the torn bytes in flash and switches the node off after that loop.
TEST(Simulator, PowerCutMidWriteTurnsTheNodeOff) {
  Simulator s(31);
  auto& n = s.add<WriterNode>("w");
  n.fs().fail_writes_after(11, SimFS::OnExhaust::POWER_OFF);
  n.writes_pending = 2;
  s.run_for(5);
  EXPECT_FALSE(n.powered());
  EXPECT_EQ(n.fs().files["/log"].size(), 11u);
  ASSERT_GE(s.fs_log().size(), 2u);
  EXPECT_TRUE(s.fs_log()[s.fs_log().size() - 2].complete);
  EXPECT_FALSE(s.fs_log().back().complete);

  n.power_on();
  EXPECT_TRUE(n.powered());
  EXPECT_EQ(n.fs().write_budget, -1) << "the fault is one-shot";
  n.writes_pending = 1;
  s.run_for(5);
  EXPECT_TRUE(n.powered());
  EXPECT_EQ(n.fs().files["/log"].size(), 19u);
}

// H3: flash writes and transmissions share one event sequence.
TEST(Simulator, FsLogAndTxLogShareOneOrder) {
  Simulator s(32);
  auto& n = s.add<WriterNode>("w");
  n.writes_pending = 1;
  n.advert_after = true;
  s.run_for(3000);
  ASSERT_EQ(s.fs_log().size(), 1u);
  ASSERT_EQ(s.tx_log().size(), 1u);
  EXPECT_EQ(s.fs_log()[0].path, "/log");
  EXPECT_EQ(s.fs_log()[0].node, n.index());
  EXPECT_LT(s.fs_log()[0].seq, s.tx_log()[0].seq);
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
