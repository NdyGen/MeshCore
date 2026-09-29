// Integration tests on the REAL companion firmware (examples/companion_radio MyMesh + DataStore) with an emulated
// phone app. They pin down current behaviour, including issue #3518.

#include <gtest/gtest.h>

#include "CompanionNode.h"

using namespace sim;
using Status = ChatNode::Status;

namespace {

// Companion default when the variant does not override it (examples/companion_radio/MyMesh.h:61-62).
// Heltec V3 BLE builds use 256 (variants/heltec_v3/platformio.ini).
constexpr int QUEUE_SIZE = OFFLINE_QUEUE_SIZE;

// Both sides advertise once (a companion only adverts when its app asks) and learn each other.
void introduce(Simulator& s, CompanionNode& alice, SimNode& bob) {
  bool was_connected = alice.app().connected();
  alice.app().connect();
  alice.app().advert(true);
  s.run_for(5000);
  if (auto chat = dynamic_cast<ChatNode*>(&bob)) chat->advert();
  if (auto comp = dynamic_cast<CompanionNode*>(&bob)) {
    comp->app().connect();
    comp->app().advert(true);
  }
  s.run_for(5000);
  if (!was_connected) alice.app().disconnect();
}

}

TEST(SimCompanion, DmBetweenTwoCompanionAppsIsConfirmedAndSynced) {
  Simulator s(11);
  auto& alice = s.add<CompanionNode>("alice");
  auto& bob = s.add<CompanionNode>("bob");
  s.link(alice, bob);
  alice.app().connect();
  introduce(s, alice, bob);
  ASSERT_TRUE(bob.app().connected());

  int id = bob.app().send_text(alice, "hi from bob's phone");
  ASSERT_TRUE(s.run_until([&] { return bob.app().sent(id).status == Status::DELIVERED; }, 30000));
  EXPECT_TRUE(bob.app().sent(id).flood);
  ASSERT_TRUE(s.run_until([&] { return alice.app().inbox_count("hi from bob's phone") == 1; }, 5000));
  EXPECT_EQ(alice.app().inbox().back().from_index, bob.index());
  EXPECT_GE(bob.app().push_count(0x82), 1u);  // PUSH_CODE_SEND_CONFIRMED
}

// Issue #3518: with the app away, the companion ACKs a DM and then drops it because the offline queue is full.
TEST(SimCompanion, Issue3518_FullOfflineQueueStillAcksAndDropsDm) {
  Simulator s(3518);
  auto& alice = s.add<CompanionNode>("alice");
  auto& bob = s.add<ChatNode>("bob");
  s.link(alice, bob);
  introduce(s, alice, bob);
  ASSERT_FALSE(alice.app().connected());
  ASSERT_TRUE(bob.knows(alice));

  const int total = QUEUE_SIZE + 1;
  int delivered = 0;
  for (int i = 0; i < total; i++) {
    int id = bob.send_text(alice, "dm #" + std::to_string(i));
    ASSERT_TRUE(s.run_until([&] { return bob.sent(id).status != Status::PENDING; }, 30000));
    delivered += bob.sent(id).status == Status::DELIVERED;
    s.run_for(2000);
  }
  EXPECT_EQ(delivered, total) << "bob's radio got an ACK for every DM";

  alice.app().connect();
  ASSERT_TRUE(s.run_until([&] { return alice.app().idle() && alice.app().inbox().size() >= (size_t)QUEUE_SIZE; }, 10000));
  s.run_for(1000);
  EXPECT_EQ(alice.app().inbox().size(), (size_t)QUEUE_SIZE);
  EXPECT_EQ(alice.app().inbox_count("dm #" + std::to_string(total - 1)), 0u) << "the last DM was ACKed but dropped";
}

// Loss mode L3: queued (and ACKed) DMs live in RAM only; a radio reboot before the app syncs loses them.
TEST(SimCompanion, RebootBeforeAppSyncLosesAckedDms) {
  Simulator s(33);
  auto& alice = s.add<CompanionNode>("alice");
  auto& bob = s.add<ChatNode>("bob");
  s.link(alice, bob);
  introduce(s, alice, bob);

  for (int i = 0; i < 3; i++) {
    int id = bob.send_text(alice, "before reboot " + std::to_string(i));
    ASSERT_TRUE(s.run_until([&] { return bob.sent(id).status == Status::DELIVERED; }, 30000));
    s.run_for(2000);
  }
  s.run_for(10000);  // lets the lazy contacts write (LAZY_CONTACTS_WRITE_DELAY) reach flash

  alice.power_off();
  s.run_for(10 * 60 * 1000);
  alice.power_on();
  // No RTC battery, but the companion bootstraps its clock from the newest contact lastmod
  // (examples/companion_radio/MyMesh.cpp:1001): it lags by the downtime instead of jumping back to 2024.
  const uint32_t t = alice.rtc().getCurrentTime();
  EXPECT_GT(t, RTC_RESET_EPOCH);
  EXPECT_LT(t, s.wall_epoch() - 9 * 60);
  alice.app().connect();
  s.run_for(5000);
  EXPECT_GE(alice.rtc().getCurrentTime(), s.wall_epoch() - 5);
  EXPECT_TRUE(alice.app().inbox().empty());

  // Contacts survived in flash (DataStore on SimFS), so the next DM still decrypts.
  int id = bob.send_text(alice, "after reboot");
  ASSERT_TRUE(s.run_until([&] { return bob.sent(id).status == Status::DELIVERED; }, 30000));
  ASSERT_TRUE(s.run_until([&] { return alice.app().inbox_count("after reboot") == 1; }, 5000));
}

TEST(SimCompanion, UnknownCommandGetsErrorFrameViaRawApi) {
  Simulator s(12);
  auto& alice = s.add<CompanionNode>("alice");
  alice.app().connect();
  std::vector<uint8_t> reply;
  alice.app().send_command({0xEE}, [&](const std::vector<uint8_t>& f) { reply = f; });
  ASSERT_TRUE(s.run_until([&] { return !reply.empty(); }, 5000));
  EXPECT_EQ(reply[0], 1);  // RESP_CODE_ERR
  EXPECT_EQ(reply[1], 1);  // ERR_CODE_UNSUPPORTED_CMD
}

// A multi-frame reply (START, one frame per item, END) is one command: the next command only goes after END.
TEST(SimCompanion, MultiFrameReplyEndsOnItsClosingFrame) {
  Simulator s(13);
  auto& alice = s.add<CompanionNode>("alice");
  auto& bob = s.add<ChatNode>("bob");
  auto& carol = s.add<ChatNode>("carol");
  s.link(alice, bob);
  s.link(alice, carol);
  bob.advert();
  s.run_for(5000);   // simultaneous adverts would collide at alice
  carol.advert();
  s.run_for(5000);
  alice.app().connect();
  s.run_for(2000);

  std::vector<uint8_t> codes;
  std::vector<uint8_t> next_reply;
  alice.app().send_command({4 /* CMD_GET_CONTACTS */}, [&](const std::vector<uint8_t>& f) { codes.push_back(f[0]); });
  alice.app().send_command({0xEE}, [&](const std::vector<uint8_t>& f) { next_reply = f; });
  ASSERT_TRUE(s.run_until([&] { return !next_reply.empty(); }, 10000));
  EXPECT_EQ(codes, (std::vector<uint8_t>{2, 3, 3, 4})) << "CONTACTS_START, 2x CONTACT, END_OF_CONTACTS";
  EXPECT_EQ(next_reply[0], 1) << "the unknown command got its own ERR, not a CONTACT frame";
  EXPECT_TRUE(alice.app().idle());
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
