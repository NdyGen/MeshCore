// Node + RdmChatMesh on real packets in the simulator (06 par. 5, WP5: S01 with two RdmSimCompanions).

#include <gtest/gtest.h>

#include <RdmSimCompanion.h>

using namespace sim;
using rdm::UserStatus;

namespace {

void introduce(Simulator& s, RdmSimCompanion& a, RdmSimCompanion& b) {
  a.advert();
  s.run_for(5000);
  b.advert();
  s.run_for(5000);
}

bool hasStatus(const RdmSimCompanion& n, uint32_t app_ack, UserStatus st) {
  for (auto x : n.statuses_for(app_ack))
    if (x == st) return true;
  return false;
}

// First transmission of `from` at or after `t` that carries an ACK (plain ACK, or PATH with the ACK inside).
const TxRecord* firstAckTx(const Simulator& s, int from, uint64_t t) {
  for (const auto& tx : s.tx_log())
    if (tx.from == from && tx.t_ms >= t &&
        (tx.payload_type == PAYLOAD_TYPE_ACK || tx.payload_type == PAYLOAD_TYPE_PATH)) return &tx;
  return nullptr;
}

}

// 05 scenario 1 on B-R-A, with its precondition: Bob has a path to Alice (and she to him).
TEST(RdmSim, S01_Basis) {
  Simulator s(501);
  auto& bob = s.add<RdmSimCompanion>("bob");
  auto& rep = s.add<RepeaterNode>("rep");
  auto& alice = s.add<RdmSimCompanion>("alice");
  s.link(bob, rep);
  s.link(rep, alice);
  bob.connect();
  alice.connect();
  introduce(s, alice, bob);
  int warm = bob.send_text(alice, "warm-up");
  ASSERT_TRUE(s.run_until([&] { return hasStatus(bob, bob.sent(warm).app_ack, UserStatus::DELIVERED); }, 60000));
  s.run_for(10000);
  ASSERT_TRUE(bob.has_path_to(alice));
  ASSERT_TRUE(alice.has_path_to(bob));

  const uint64_t t0 = s.now();
  const size_t confirms_before = bob.confirms().size();
  int id = bob.send_text(alice, "S01 basis");
  ASSERT_GE(id, 0);
  ASSERT_TRUE(bob.sent(id).handled);
  const uint32_t ack = bob.sent(id).app_ack;
  ASSERT_TRUE(s.run_until([&] { return hasStatus(bob, ack, UserStatus::DELIVERED); }, 60000));

  EXPECT_EQ(bob.statuses_for(ack), (std::vector<UserStatus>{UserStatus::QUEUED, UserStatus::ON_RADIO, UserStatus::DELIVERED}));
  ASSERT_EQ(bob.confirms().size(), confirms_before + 1) << "SEND_CONFIRMED once";
  EXPECT_EQ(bob.confirms().back(), ack);
  EXPECT_EQ(alice.inbox_count("S01 basis"), 1u);

  // ACK_R only after the inbox write reached flash
  const TxRecord* ack_tx = firstAckTx(s, alice.index(), t0);
  ASSERT_NE(ack_tx, nullptr);
  EXPECT_EQ(ack_tx->payload_type, PAYLOAD_TYPE_ACK);
  EXPECT_EQ(ack_tx->raw.size(), 2u + 1u + 7u) << "header, path (1 hop), ACK_R with cap byte";
  uint64_t inbox_seq = UINT64_MAX;
  for (const auto& w : s.fs_log())
    if (w.node == alice.index() && w.t_ms >= t0 && w.path == "/rdm/inbox" && w.complete) {
      inbox_seq = w.seq;
      break;
    }
  ASSERT_NE(inbox_seq, UINT64_MAX);
  EXPECT_LT(inbox_seq, ack_tx->seq);

  s.run_for(10 * 60 * 1000);
  EXPECT_EQ(alice.inbox_count("S01 basis"), 1u);
  EXPECT_EQ(bob.statuses_for(ack).size(), 3u);
}

// Without a path the first DM floods and ACK_R rides in the PATH return (priority 2); with an app that syncs at
// once, ACK_S (flood ACK, priority 1) can go first. Bob then skips ON_RADIO but ends DELIVERED with one confirm.
TEST(RdmSim, S01_FirstDmWithoutPathEndsDelivered) {
  Simulator s(501);
  auto& bob = s.add<RdmSimCompanion>("bob");
  auto& rep = s.add<RepeaterNode>("rep");
  auto& alice = s.add<RdmSimCompanion>("alice");
  s.link(bob, rep);
  s.link(rep, alice);
  bob.connect();
  alice.connect();
  introduce(s, alice, bob);

  int id = bob.send_text(alice, "no path yet");
  const uint32_t ack = bob.sent(id).app_ack;
  ASSERT_TRUE(s.run_until([&] { return hasStatus(bob, ack, UserStatus::DELIVERED); }, 60000));
  EXPECT_EQ(bob.statuses_for(ack).front(), UserStatus::QUEUED);
  EXPECT_EQ(bob.statuses_for(ack).back(), UserStatus::DELIVERED);
  EXPECT_EQ(bob.confirms(), std::vector<uint32_t>{ack});
  s.run_for(10 * 60 * 1000);
  EXPECT_EQ(alice.inbox_count("no path yet"), 1u);
  EXPECT_TRUE(bob.has_path_to(alice));
}

// 05 scenario 1: 157-character DM with known paths, one hop: 1.72 + 0.28 + 0.28 = 2.3 s (+- 20%).
TEST(RdmSim, S01_AirtimePerHop) {
  Simulator s(502);
  auto& bob = s.add<RdmSimCompanion>("bob");
  auto& alice = s.add<RdmSimCompanion>("alice");
  s.link(bob, alice);
  bob.connect();
  alice.connect();
  introduce(s, alice, bob);
  int warm = bob.send_text(alice, "warm-up: paths both ways");
  ASSERT_TRUE(s.run_until([&] { return hasStatus(bob, bob.sent(warm).app_ack, UserStatus::DELIVERED); }, 60000));
  s.run_for(10000);
  ASSERT_TRUE(bob.has_path_to(alice));
  ASSERT_TRUE(alice.has_path_to(bob));

  const uint64_t t0 = s.now();
  int id = bob.send_text(alice, std::string(rdm::MAX_TEXT, 'x'));
  const uint32_t ack = bob.sent(id).app_ack;
  ASSERT_TRUE(s.run_until([&] { return hasStatus(bob, ack, UserStatus::DELIVERED); }, 60000));
  s.run_for(30000);
  uint32_t air = 0;
  for (const auto& tx : s.tx_log())
    if (tx.t_ms >= t0) air += tx.airtime_ms;
  EXPECT_NEAR(air, 2300, 460) << "DM + ACK_R + ACK_S";
}

// Runtime off on Alice: stock companion behaviour, Bob stops at ON_RADIO_FINAL.
TEST(RdmSim, StockAliceGetsAnUpstreamAckWithoutCap) {
  Simulator s(503);
  auto& bob = s.add<RdmSimCompanion>("bob");
  auto& alice = s.add<RdmSimCompanion>("alice");
  s.link(bob, alice);
  alice.rdm().setEnabled(false);
  bob.connect();
  alice.connect();
  introduce(s, alice, bob);

  const uint64_t t0 = s.now();
  int id = bob.send_text(alice, "to a stock companion");
  const uint32_t ack = bob.sent(id).app_ack;
  ASSERT_TRUE(s.run_until([&] { return hasStatus(bob, ack, UserStatus::ON_RADIO_FINAL); }, 60000));
  EXPECT_EQ(bob.confirms(), std::vector<uint32_t>{ack});
  EXPECT_EQ(alice.inbox_count("to a stock companion"), 1u) << "via the upstream queue, trailer not shown";
  EXPECT_TRUE(alice.fs().files.count("/rdm/inbox") == 0 || !alice.inbox().back().via_rdm);

  const TxRecord* ack_tx = firstAckTx(s, alice.index(), t0);
  ASSERT_NE(ack_tx, nullptr);
  EXPECT_EQ(ack_tx->payload_type, PAYLOAD_TYPE_PATH) << "first DM without path: ACK in the PATH return";
  s.set_fast_forward(1000);
  s.run_for(3 * 3600 * 1000);
  EXPECT_EQ(s.count_tx(bob.index(), PAYLOAD_TYPE_TXT_MSG), 1u) << "nothing more after ON_RADIO_FINAL";
  EXPECT_EQ(s.count_tx(bob.index(), PAYLOAD_TYPE_REQ), 0u);
}

// H4: S02 (Alice' phone away) gives the same transmissions with and without fast-forward. 6 h instead of 13 h
// keeps the 1 ms reference run short; it still covers two RECEIPT_QUERYs (ON_RADIO schedule +1 h, +5 h) and the sync.
TEST(RdmSim, S02_FastForwardGivesTheSameRun) {
  auto run = [](bool ff, std::vector<UserStatus>& st, size_t& shown) {
    Simulator s(504);
    if (ff) s.set_fast_forward(1000);
    auto& bob = s.add<RdmSimCompanion>("bob");
    auto& alice = s.add<RdmSimCompanion>("alice");
    s.link(bob, alice);
    bob.connect();
    alice.connect();
    introduce(s, alice, bob);
    alice.disconnect();
    int id = bob.send_text(alice, "while your phone is away");
    s.run_for(6ull * 3600 * 1000);
    alice.connect();
    s.run_for(60000);
    st = bob.statuses_for(bob.sent(id).app_ack);
    shown = alice.inbox_count("while your phone is away");
    return s.tx_log();
  };
  std::vector<UserStatus> st_slow, st_fast;
  size_t shown_slow = 0, shown_fast = 0;
  auto slow = run(false, st_slow, shown_slow);
  auto fast = run(true, st_fast, shown_fast);

  EXPECT_EQ(st_slow, (std::vector<UserStatus>{UserStatus::QUEUED, UserStatus::ON_RADIO, UserStatus::DELIVERED}));
  EXPECT_EQ(shown_slow, 1u);
  size_t queries = 0;
  for (const auto& tx : slow) queries += tx.payload_type == PAYLOAD_TYPE_REQ;
  EXPECT_EQ(queries, 2u) << "RECEIPT_QUERY at +1 h and +5 h (RDM_SCHED_ON_RADIO is cumulative)";

  EXPECT_EQ(st_fast, st_slow);
  EXPECT_EQ(shown_fast, shown_slow);
  ASSERT_EQ(fast.size(), slow.size());
  for (size_t i = 0; i < slow.size(); i++) {
    EXPECT_EQ(fast[i].t_ms, slow[i].t_ms) << "tx " << i;
    EXPECT_EQ(fast[i].raw, slow[i].raw) << "tx " << i;
  }
}

// ---- K3: a mailbox is a hidden peer, never a contact ----------------------------------------------------------

namespace {

const uint8_t K_OWNER[16] = {3, 1, 4, 1, 5, 9, 2, 6, 5, 3, 5, 8, 9, 7, 9, 3};

bool samePub(const std::vector<uint8_t>& a, const SimNode& n) {
  return a.size() == PUB_KEY_SIZE && memcmp(a.data(), n.identity().pub_key, PUB_KEY_SIZE) == 0;
}

// Bob and Alice (forks, favourites of each other) and M; Alice registers M as her mailbox, Bob has heard M first.
struct K3World {
  Simulator s;
  RdmSimCompanion& bob;
  RdmSimCompanion& alice;
  ChatNode& mbx;   // stands in for the mailbox: only its identity and advert matter here

  explicit K3World(uint64_t seed)
      : s(seed), bob(s.add<RdmSimCompanion>("bob")), alice(s.add<RdmSimCompanion>("alice")), mbx(s.add<ChatNode>("mbx")) {
    s.link(bob, alice);
    s.link(bob, mbx);
    s.link(alice, mbx);
    bob.connect();
    alice.connect();
    alice.advert();
    s.run_for(5000);
    bob.advert();
    s.run_for(5000);
    mbx.advert();
    s.run_for(5000);
    alice.add_contact(bob, true);
    bob.add_contact(alice, true);
  }

  // Alice's MBX_INFO reaches Bob with her next reply to a fork DM from him.
  void mbxInfoToBob() {
    ASSERT_TRUE(alice.set_mailbox(mbx.identity().pub_key, K_OWNER));
    int id = bob.send_text(alice, "hi");
    ASSERT_GE(id, 0);
    ASSERT_TRUE(s.run_until([&] { return bob.rdm().hiddenPeerCount() == 1; }, 5 * 60 * 1000));
  }
};

}

TEST(RdmSimK3, OwnMailboxAdvertIsNoContactButTriggersFetch) {
  Simulator s(601);
  auto& alice = s.add<RdmSimCompanion>("alice");
  auto& bob = s.add<ChatNode>("bob");
  auto& mbx = s.add<ChatNode>("mbx");
  s.link(alice, mbx);
  s.link(alice, bob);
  alice.connect();
  ASSERT_TRUE(alice.set_mailbox(mbx.identity().pub_key, K_OWNER));
  s.run_for(15 * 60 * 1000);   // boot/trigger FETCHes done and > 10 min old; next one only at the 60 min interval
  const size_t fetches = s.count_tx(alice.index(), PAYLOAD_TYPE_REQ);
  ASSERT_GE(fetches, 1u);

  mbx.advert();
  s.run_for(10000);
  EXPECT_FALSE(alice.knows(mbx));
  EXPECT_TRUE(alice.removed_contacts().empty()) << "never a contact, so nothing to remove";
  EXPECT_EQ(s.count_tx(alice.index(), PAYLOAD_TYPE_REQ), fetches + 1) << "the advert triggers a FETCH";

  mbx.advert();   // within 10 min of that FETCH: no new one
  s.run_for(10000);
  EXPECT_EQ(s.count_tx(alice.index(), PAYLOAD_TYPE_REQ), fetches + 1);

  bob.advert();   // an ordinary advert still makes a contact
  s.run_for(10000);
  EXPECT_TRUE(alice.knows(bob));
}

TEST(RdmSimK3, MailboxLeavesTheSendersContactsAfterMbxInfo) {
  K3World w(602);
  ASSERT_TRUE(w.bob.knows(w.mbx)) << "heard before MBX_INFO: auto-added";
  w.mbxInfoToBob();

  EXPECT_FALSE(w.bob.knows(w.mbx));
  ASSERT_EQ(w.bob.removed_contacts().size(), 1u);
  EXPECT_TRUE(samePub(w.bob.removed_contacts()[0], w.mbx));
  EXPECT_TRUE(w.bob.knows(w.alice));

  w.mbx.advert();
  w.s.run_for(10000);
  EXPECT_FALSE(w.bob.knows(w.mbx)) << "its advert no longer adds it";
  EXPECT_EQ(w.bob.removed_contacts().size(), 1u);

  w.bob.reboot();
  EXPECT_FALSE(w.bob.knows(w.mbx)) << "removal was persisted";
  EXPECT_EQ(w.bob.removed_contacts().size(), 1u);
}

TEST(RdmSimK3, StaleContactOfTheOwnMailboxIsRemovedAtBoot) {
  Simulator s(603);
  auto& alice = s.add<RdmSimCompanion>("alice");
  auto& mbx = s.add<ChatNode>("mbx");
  s.link(alice, mbx);
  ASSERT_TRUE(alice.set_mailbox(mbx.identity().pub_key, K_OWNER));
  alice.add_contact(mbx, true);   // the state an older firmware left in /contacts, favourite even
  ASSERT_TRUE(alice.knows(mbx));

  alice.reboot();
  EXPECT_FALSE(alice.knows(mbx));
  ASSERT_EQ(alice.removed_contacts().size(), 1u);
  EXPECT_TRUE(samePub(alice.removed_contacts()[0], mbx));
}

TEST(RdmSimK3, RevokeTurnsTheMailboxIntoAnOrdinaryAdvertAgain) {
  K3World w(604);
  w.mbxInfoToBob();
  ASSERT_FALSE(w.bob.knows(w.mbx));
  // Revoked right away: Bob's ACK of MBX_INFO may still be on its way, the REVOKE must reach him anyway.

  ASSERT_TRUE(w.alice.set_mailbox(nullptr, nullptr));
  ASSERT_TRUE(w.s.run_until([&] { return w.bob.rdm().hiddenPeerCount() == 0; }, 5 * 60 * 1000)) << "MBX_REVOKE";
  EXPECT_EQ(w.alice.rdm().hiddenPeerCount(), 0);

  w.mbx.advert();
  w.s.run_for(10000);
  EXPECT_TRUE(w.bob.knows(w.mbx));
  EXPECT_TRUE(w.alice.knows(w.mbx));
  EXPECT_EQ(w.bob.removed_contacts().size(), 1u);
}
