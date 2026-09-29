// Integration tests of CURRENT (upstream) DM behaviour on the native multi-node simulator.
// They document the baseline that reliable DM has to improve on (docs/reliable-dm/01-huidige-werking.md).

#include <gtest/gtest.h>

#include "ChatNode.h"

using namespace sim;
using Status = ChatNode::Status;

namespace {

void exchangeAdverts(Simulator& s, ChatNode& a, ChatNode& b) {
  a.advert();
  s.run_for(5000);
  b.advert();
  s.run_for(5000);
}

bool delivered(ChatNode& n, int id) { return n.sent(id).status == Status::DELIVERED; }

}

TEST(SimDmBaseline, NeighboursFloodThenDirectDmAreAcked) {
  Simulator s(42);
  auto& alice = s.add<ChatNode>("alice");
  auto& bob = s.add<ChatNode>("bob");
  s.link(alice, bob);

  exchangeAdverts(s, alice, bob);
  ASSERT_TRUE(bob.knows(alice));
  ASSERT_TRUE(alice.knows(bob));
  ASSERT_FALSE(bob.has_path_to(alice));

  int m1 = bob.send_text(alice, "hello alice");
  ASSERT_GE(m1, 0);
  EXPECT_TRUE(bob.sent(m1).flood);
  ASSERT_TRUE(s.run_until([&] { return delivered(bob, m1); }, 30000));
  ASSERT_EQ(alice.inbox_count("hello alice"), 1u);
  EXPECT_EQ(alice.inbox().back().from_index, bob.index());
  EXPECT_TRUE(alice.inbox().back().flood);
  EXPECT_FALSE(bob.sent(m1).timed_out_before_ack);

  // The flood DM is answered with a PATH packet carrying the ACK, so bob learns a (zero-hop) direct path.
  EXPECT_GE(s.count_tx(alice.index(), PAYLOAD_TYPE_PATH), 1u);
  ASSERT_TRUE(bob.has_path_to(alice));
  EXPECT_EQ(bob.path_len_to(alice) & 63, 0);
  s.run_for(5000);  // let bob's reciprocal PATH reach alice, as a human typing the next message would
  EXPECT_TRUE(alice.has_path_to(bob));

  int m2 = bob.send_text(alice, "second, direct");
  EXPECT_FALSE(bob.sent(m2).flood);
  ASSERT_TRUE(s.run_until([&] { return delivered(bob, m2); }, 30000));
  EXPECT_EQ(alice.inbox_count("second, direct"), 1u);
  EXPECT_FALSE(alice.inbox().back().flood);
}

TEST(SimDmBaseline, TwoHopDmViaRepeater) {
  Simulator s(7);
  auto& alice = s.add<ChatNode>("alice");
  auto& rep = s.add<RepeaterNode>("repeater");
  auto& bob = s.add<ChatNode>("bob");
  s.link(bob, rep);
  s.link(rep, alice);
  ASSERT_FALSE(s.linked(bob, alice));

  exchangeAdverts(s, alice, bob);
  ASSERT_TRUE(bob.knows(alice));
  ASSERT_TRUE(alice.knows(bob));

  int m1 = bob.send_text(alice, "via repeater");
  ASSERT_TRUE(s.run_until([&] { return delivered(bob, m1); }, 60000));
  ASSERT_EQ(alice.inbox_count("via repeater"), 1u);
  EXPECT_EQ(alice.inbox().back().path_len & 63, 1);  // one repeater hash in the flood path
  EXPECT_GE(s.count_tx(rep.index(), PAYLOAD_TYPE_TXT_MSG), 1u);

  ASSERT_TRUE(bob.has_path_to(alice));
  EXPECT_EQ(bob.path_len_to(alice) & 63, 1);
  // Sending immediately would make bob's reciprocal PATH and alice's flood ACK collide at the repeater
  // (hidden terminals); see SimDmBaseline.HiddenTerminalCollisionLosesAck.
  s.run_for(5000);
  EXPECT_TRUE(alice.has_path_to(bob));

  int m2 = bob.send_text(alice, "direct via repeater");
  EXPECT_FALSE(bob.sent(m2).flood);
  ASSERT_TRUE(s.run_until([&] { return delivered(bob, m2); }, 60000));
  EXPECT_EQ(alice.inbox_count("direct via repeater"), 1u);
}

// Documents a real effect the simulator surfaced: a DM sent right after the first ACK goes direct while the
// recipient still floods its ACK, and that ACK collides at the repeater with the sender's reciprocal PATH.
TEST(SimDmBaseline, HiddenTerminalCollisionLosesAck) {
  Simulator s(7);
  auto& alice = s.add<ChatNode>("alice");
  auto& rep = s.add<RepeaterNode>("repeater");
  auto& bob = s.add<ChatNode>("bob");
  s.link(bob, rep);
  s.link(rep, alice);
  exchangeAdverts(s, alice, bob);

  int m1 = bob.send_text(alice, "first");
  ASSERT_TRUE(s.run_until([&] { return delivered(bob, m1); }, 60000));
  int m2 = bob.send_text(alice, "second, immediately");
  s.run_for(60000);

  EXPECT_EQ(alice.inbox_count("second, immediately"), 1u) << "the DM itself arrives";
  EXPECT_EQ(bob.sent(m2).status, Status::TIMED_OUT) << "but bob never sees the ACK";
  EXPECT_GE(s.dropped_collision(), 1u);
}

// Baseline for the problem: nothing stores a DM for a recipient whose radio is off (loss mode L1).
TEST(SimDmBaseline, DmIsLostWhenRecipientRadioIsOff) {
  Simulator s(3);
  auto& alice = s.add<ChatNode>("alice");
  auto& bob = s.add<ChatNode>("bob");
  s.link(alice, bob);
  exchangeAdverts(s, alice, bob);
  ASSERT_TRUE(bob.knows(alice));

  alice.power_off();
  int job = bob.send_text_with_retries(alice, "are you there?");
  ASSERT_GE(job, 0);
  ASSERT_TRUE(s.run_until([&] { return bob.retry_done(job); }, 120000));
  EXPECT_EQ(bob.retry_status(job), Status::TIMED_OUT);
  EXPECT_EQ(s.count_tx(bob.index(), PAYLOAD_TYPE_TXT_MSG), 3u);

  const size_t txt_before = s.count_tx(-1, PAYLOAD_TYPE_TXT_MSG);
  alice.power_on();
  alice.sync_clock();
  alice.advert();
  s.run_for(10 * 60 * 1000);

  EXPECT_EQ(alice.inbox_count("are you there?"), 0u);
  EXPECT_EQ(s.count_tx(-1, PAYLOAD_TYPE_TXT_MSG), txt_before) << "nobody retransmits the DM once alice is back";
  EXPECT_NE(bob.retry_status(job), Status::DELIVERED);
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
