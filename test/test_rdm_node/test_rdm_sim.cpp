// Node + RdmChatMesh on real packets in the simulator (06 par. 5: S01 with two companions on the real MyMesh,
// driven through companion frames; the white-box view comes from the app's push log and the firmware).

#include <gtest/gtest.h>

#include <RdmScenario.h>
#include <RdmSimApp.h>

using namespace sim;
using rdm::UserStatus;

namespace {

// The app connects and, as an RDM client, sends CMD_RDM_ENABLE.
void connectRdm(CompanionNode& n) {
  n.app().connect();
  ASSERT_TRUE(enableRdm(n)) << n.name() << ": CMD_RDM_ENABLE";
}

void introduce(Simulator& s, CompanionNode& a, CompanionNode& b) {
  a.app().advert(true);
  s.run_for(5000);
  b.app().advert(true);
  s.run_for(5000);
}

// CMD_SEND_TXT_MSG and the run up to RESP_CODE_SENT, so expected_ack is known; -1 if the app is not connected.
int sendText(CompanionNode& from, const SimNode& to, const std::string& text, uint8_t attempt = 0, uint32_t ts = 0) {
  int id = from.app().send_text(to, text, attempt, ts);
  if (id < 0) return -1;
  EXPECT_TRUE(from.sim().run_until([&] { return from.app().sent(id).answered; }, 10000)) << "no RESP_CODE_SENT";
  return id;
}

uint32_t ackOf(CompanionNode& n, int id) { return n.app().sent(id).expected_ack; }

// The outbox took the message: an entry exists for its recipient and timestamp.
bool handled(CompanionNode& n, const SimNode& to, int id) { return outState(n, to, n.app().sent(id).timestamp) >= 0; }

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
  auto& bob = s.add<CompanionNode>("bob");
  auto& rep = s.add<RepeaterNode>("rep");
  auto& alice = s.add<CompanionNode>("alice");
  s.link(bob, rep);
  s.link(rep, alice);
  connectRdm(bob);
  connectRdm(alice);
  introduce(s, alice, bob);
  int warm = sendText(bob, alice, "warm-up");
  ASSERT_TRUE(s.run_until([&] { return hasStatus(bob.app(), ackOf(bob, warm), UserStatus::DELIVERED); }, 60000));
  s.run_for(10000);
  ASSERT_TRUE(hasPathTo(bob, alice));
  ASSERT_TRUE(hasPathTo(alice, bob));

  const uint64_t t0 = s.now();
  const size_t confirms_before = confirmsOf(bob.app()).size();
  int id = sendText(bob, alice, "S01 basis");
  ASSERT_GE(id, 0);
  ASSERT_TRUE(handled(bob, alice, id));
  const uint32_t ack = ackOf(bob, id);
  ASSERT_TRUE(s.run_until([&] { return hasStatus(bob.app(), ack, UserStatus::DELIVERED); }, 60000));

  EXPECT_EQ(statusesFor(bob.app(), ack), (std::vector<UserStatus>{UserStatus::QUEUED, UserStatus::ON_RADIO, UserStatus::DELIVERED}));
  ASSERT_EQ(confirmsOf(bob.app()).size(), confirms_before + 1) << "SEND_CONFIRMED once";
  EXPECT_EQ(confirmsOf(bob.app()).back(), ack);
  EXPECT_EQ(alice.app().inbox_count("S01 basis"), 1u);

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
  EXPECT_EQ(alice.app().inbox_count("S01 basis"), 1u);
  EXPECT_EQ(statusesFor(bob.app(), ack).size(), 3u);
}

// Without a path the first DM floods and ACK_R rides in the PATH return (priority 2); with an app that syncs at
// once, ACK_S (flood ACK, priority 1) can go first. Bob then skips ON_RADIO but ends DELIVERED with one confirm.
TEST(RdmSim, S01_FirstDmWithoutPathEndsDelivered) {
  Simulator s(501);
  auto& bob = s.add<CompanionNode>("bob");
  auto& rep = s.add<RepeaterNode>("rep");
  auto& alice = s.add<CompanionNode>("alice");
  s.link(bob, rep);
  s.link(rep, alice);
  connectRdm(bob);
  connectRdm(alice);
  introduce(s, alice, bob);

  int id = sendText(bob, alice, "no path yet");
  const uint32_t ack = ackOf(bob, id);
  ASSERT_TRUE(s.run_until([&] { return hasStatus(bob.app(), ack, UserStatus::DELIVERED); }, 60000));
  EXPECT_EQ(statusesFor(bob.app(), ack).front(), UserStatus::QUEUED);
  EXPECT_EQ(statusesFor(bob.app(), ack).back(), UserStatus::DELIVERED);
  EXPECT_EQ(confirmsOf(bob.app()), std::vector<uint32_t>{ack});
  s.run_for(10 * 60 * 1000);
  EXPECT_EQ(alice.app().inbox_count("no path yet"), 1u);
  EXPECT_TRUE(hasPathTo(bob, alice));
}

// 05 scenario 1: 157-character DM with known paths, one hop: 1.72 + 0.28 + 0.28 = 2.3 s (+- 20%).
TEST(RdmSim, S01_AirtimePerHop) {
  Simulator s(502);
  auto& bob = s.add<CompanionNode>("bob");
  auto& alice = s.add<CompanionNode>("alice");
  s.link(bob, alice);
  connectRdm(bob);
  connectRdm(alice);
  introduce(s, alice, bob);
  int warm = sendText(bob, alice, "warm-up: paths both ways");
  ASSERT_TRUE(s.run_until([&] { return hasStatus(bob.app(), ackOf(bob, warm), UserStatus::DELIVERED); }, 60000));
  s.run_for(10000);
  ASSERT_TRUE(hasPathTo(bob, alice));
  ASSERT_TRUE(hasPathTo(alice, bob));

  const uint64_t t0 = s.now();
  int id = sendText(bob, alice, std::string(rdm::MAX_TEXT, 'x'));
  const uint32_t ack = ackOf(bob, id);
  ASSERT_TRUE(s.run_until([&] { return hasStatus(bob.app(), ack, UserStatus::DELIVERED); }, 60000));
  s.run_for(30000);
  uint32_t air = 0;
  for (const auto& tx : s.tx_log())
    if (tx.t_ms >= t0) air += tx.airtime_ms;
  EXPECT_NEAR(air, 2300, 460) << "DM + ACK_R + ACK_S";
}

// Runtime off on Alice: stock companion behaviour, Bob stops at ON_RADIO_FINAL.
TEST(RdmSim, StockAliceGetsAnUpstreamAckWithoutCap) {
  Simulator s(503);
  auto& bob = s.add<CompanionNode>("bob");
  auto& alice = s.add<CompanionNode>("alice");
  s.link(bob, alice);
  alice.firmware().rdm().setEnabled(false);
  connectRdm(bob);
  alice.app().connect();   // stock firmware refuses CMD_RDM_ENABLE (BAD_STATE): the standard app
  introduce(s, alice, bob);

  const uint64_t t0 = s.now();
  const std::string text = "to a stock companion";
  int id = sendText(bob, alice, text);
  const uint32_t ack = ackOf(bob, id);
  ASSERT_TRUE(s.run_until([&] { return hasStatus(bob.app(), ack, UserStatus::ON_RADIO_FINAL); }, 60000));
  EXPECT_EQ(confirmsOf(bob.app()), std::vector<uint32_t>{ack});
  EXPECT_EQ(alice.app().inbox_count(text), 1u) << "via the upstream queue, trailer not shown";
  EXPECT_FALSE(fileContains(alice.fs(), "/rdm/inbox", text)) << "not stored by RDM";

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
    auto& bob = s.add<CompanionNode>("bob");
    auto& alice = s.add<CompanionNode>("alice");
    s.link(bob, alice);
    connectRdm(bob);
    connectRdm(alice);
    introduce(s, alice, bob);
    alice.app().disconnect();
    int id = sendText(bob, alice, "while your phone is away");
    s.run_for(6ull * 3600 * 1000);
    connectRdm(alice);
    s.run_for(60000);
    st = statusesFor(bob.app(), ackOf(bob, id));
    shown = alice.app().inbox_count("while your phone is away");
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

// CMD_ADD_UPDATE_CONTACT as the app sends it, answered before the caller goes on.
void addContactNow(CompanionNode& n, const SimNode& other, bool favourite) {
  ASSERT_TRUE(commandOk(n, addContactFrame(n, other, favourite))) << n.name() << ": CMD_ADD_UPDATE_CONTACT";
}

// Bob and Alice (forks, favourites of each other) and M; Alice registers M as her mailbox, Bob has heard M first.
struct K3World {
  Simulator s;
  CompanionNode& bob;
  CompanionNode& alice;
  ChatNode& mbx;   // stands in for the mailbox: only its identity and advert matter here

  explicit K3World(uint64_t seed)
      : s(seed), bob(s.add<CompanionNode>("bob")), alice(s.add<CompanionNode>("alice")), mbx(s.add<ChatNode>("mbx")) {
    s.link(bob, alice);
    s.link(bob, mbx);
    s.link(alice, mbx);
    connectRdm(bob);
    connectRdm(alice);
    alice.app().advert(true);
    s.run_for(5000);
    bob.app().advert(true);
    s.run_for(5000);
    mbx.advert();
    s.run_for(5000);
    addContactNow(alice, bob, true);
    addContactNow(bob, alice, true);
  }

  // Alice's MBX_INFO reaches Bob with her next reply to a fork DM from him.
  void mbxInfoToBob() {
    ASSERT_TRUE(setMailbox(alice, mbx.identity().pub_key, K_OWNER));
    int id = sendText(bob, alice, "hi");
    ASSERT_GE(id, 0);
    ASSERT_TRUE(s.run_until([&] { return bob.firmware().rdm().hiddenPeerCount() == 1; }, 5 * 60 * 1000));
  }
};

}

TEST(RdmSimK3, OwnMailboxAdvertIsNoContactButTriggersFetch) {
  Simulator s(601);
  auto& alice = s.add<CompanionNode>("alice");
  auto& bob = s.add<ChatNode>("bob");
  auto& mbx = s.add<ChatNode>("mbx");
  s.link(alice, mbx);
  s.link(alice, bob);
  connectRdm(alice);
  ASSERT_TRUE(setMailbox(alice, mbx.identity().pub_key, K_OWNER));
  s.run_for(15 * 60 * 1000);   // boot/trigger FETCHes done and > 10 min old; next one only at the 60 min interval
  const size_t fetches = s.count_tx(alice.index(), PAYLOAD_TYPE_REQ);
  ASSERT_GE(fetches, 1u);

  mbx.advert();
  s.run_for(10000);
  EXPECT_FALSE(isContact(alice, mbx));
  EXPECT_TRUE(removedContacts(alice.app()).empty()) << "never a contact, so nothing to remove";
  EXPECT_EQ(s.count_tx(alice.index(), PAYLOAD_TYPE_REQ), fetches + 1) << "the advert triggers a FETCH";

  mbx.advert();   // within 10 min of that FETCH: no new one
  s.run_for(10000);
  EXPECT_EQ(s.count_tx(alice.index(), PAYLOAD_TYPE_REQ), fetches + 1);

  bob.advert();   // an ordinary advert still makes a contact
  s.run_for(10000);
  EXPECT_TRUE(isContact(alice, bob));
}

TEST(RdmSimK3, MailboxLeavesTheSendersContactsAfterMbxInfo) {
  K3World w(602);
  ASSERT_TRUE(isContact(w.bob, w.mbx)) << "heard before MBX_INFO: auto-added";
  const int contacts_before = contactCount(w.bob);
  w.mbxInfoToBob();
  w.s.run_for(100);   // the CONTACT_DELETED push reaches the app

  EXPECT_FALSE(isContact(w.bob, w.mbx));
  ASSERT_EQ(removedContacts(w.bob.app()).size(), 1u);
  EXPECT_TRUE(samePub(removedContacts(w.bob.app())[0], w.mbx));
  EXPECT_TRUE(isContact(w.bob, w.alice));
  EXPECT_EQ(contactCount(w.bob), contacts_before - 1);

  w.mbx.advert();
  w.s.run_for(10000);
  EXPECT_FALSE(isContact(w.bob, w.mbx)) << "its advert no longer adds it";
  EXPECT_EQ(removedContacts(w.bob.app()).size(), 1u);
  EXPECT_EQ(contactCount(w.bob), contacts_before - 1);

  w.s.run_for(10000);   // lazy contacts write before the reboot
  w.bob.reboot();
  EXPECT_FALSE(isContact(w.bob, w.mbx)) << "removal was persisted";
  EXPECT_TRUE(isContact(w.bob, w.alice));
  EXPECT_EQ(contactCount(w.bob), contacts_before - 1) << "nothing else removed at boot";
  EXPECT_EQ(removedContacts(w.bob.app()).size(), 1u);
}

TEST(RdmSimK3, StaleContactOfTheOwnMailboxIsRemovedAtBoot) {
  Simulator s(603);
  auto& alice = s.add<CompanionNode>("alice");
  auto& mbx = s.add<ChatNode>("mbx");
  s.link(alice, mbx);
  connectRdm(alice);
  ASSERT_TRUE(setMailbox(alice, mbx.identity().pub_key, K_OWNER));
  addContactNow(alice, mbx, true);   // the state an older firmware left in /contacts, favourite even
  ASSERT_TRUE(isContact(alice, mbx));
  s.run_for(10000);   // lazy contacts write, so the stale contact is in flash at the next boot
  ASSERT_EQ(contactCount(alice), 1);

  // The removal happens in begin(), before the app link exists, so no CONTACT_DELETED push can carry it: the
  // evidence is the contact list itself, which loses exactly the mailbox, and its persisted form.
  alice.reboot();
  EXPECT_FALSE(isContact(alice, mbx));
  EXPECT_EQ(contactCount(alice), 0);
  s.run_for(10000);   // lazy contacts write
  EXPECT_FALSE(fileContains(alice.fs(), "/contacts3", std::string((const char*)mbx.identity().pub_key, PUB_KEY_SIZE)))
      << "the mailbox is still in the contacts file";
}

TEST(RdmSimK3, RevokeTurnsTheMailboxIntoAnOrdinaryAdvertAgain) {
  K3World w(604);
  w.mbxInfoToBob();
  ASSERT_FALSE(isContact(w.bob, w.mbx));
  // Revoked right away: Bob's ACK of MBX_INFO may still be on its way, the REVOKE must reach him anyway.

  ASSERT_TRUE(setMailbox(w.alice, nullptr, nullptr));
  ASSERT_TRUE(w.s.run_until([&] { return w.bob.firmware().rdm().hiddenPeerCount() == 0; }, 5 * 60 * 1000)) << "MBX_REVOKE";
  EXPECT_EQ(w.alice.firmware().rdm().hiddenPeerCount(), 0);

  w.mbx.advert();
  w.s.run_for(10000);
  EXPECT_TRUE(isContact(w.bob, w.mbx));
  EXPECT_TRUE(isContact(w.alice, w.mbx));
  EXPECT_EQ(removedContacts(w.bob.app()).size(), 1u);
}
