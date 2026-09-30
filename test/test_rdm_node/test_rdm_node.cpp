// rdm::Node with the real Outbox, Inbox, Fetcher, Clock and ContactTable on a fake wire (06 par. 6.1).

#include <gtest/gtest.h>

#include "wire.h"

using namespace rdm;
using wire::Kind;
using wire::Net;
using wire::Peer;

namespace {

uint32_t upstreamExpectedAck(uint32_t ts, uint8_t attempt, const std::string& text, const uint8_t* sender_pub) {
  uint8_t a[4];
  crypto::ackR(a, ts, attempt & 3, text.data(), text.size(), sender_pub);
  uint32_t v;
  memcpy(&v, a, 4);
  return v;
}

std::vector<UserStatus> statusesOf(const Peer& p, uint32_t app_ack) {
  std::vector<UserStatus> out;
  for (const auto& s : p.statuses)
    if (s.app_ack == app_ack) out.push_back(s.s);
  return out;
}

struct Pair {
  Net net;
  Peer& bob;
  Peer& alice;
  Pair() : bob(net.add()), alice(net.add()) {
    net.befriend(bob, alice);
    bob.boot();
    alice.boot();
  }
};

}

TEST(RdmNode, SenderReachesDeliveredAndRecipientShowsOnce) {
  Pair p;
  const uint32_t ts = p.net.epoch + 5;
  Node::AppSend a = p.bob.appSend(p.alice, "hello alice", 0, ts);
  ASSERT_TRUE(a.handled);
  EXPECT_TRUE(a.sent);
  EXPECT_EQ(a.est_timeout_ms, 2000u) << "the wire's sendTxtPlain estimate";
  EXPECT_EQ(p.net.log.back().bytes.size(), 5u + 11u + 3u) << "cap trailer";
  EXPECT_EQ(a.app_ack, upstreamExpectedAck(ts, 0, "hello alice", p.bob.pub)) << "standard app matches on this";

  p.net.pump();
  EXPECT_EQ(p.bob.confirms, std::vector<uint32_t>{a.app_ack});
  EXPECT_EQ(statusesOf(p.bob, a.app_ack), (std::vector<UserStatus>{UserStatus::QUEUED, UserStatus::ON_RADIO}));
  EXPECT_GE(p.alice.msg_waiting, 1);
  const auto& ack = p.net.log.back();
  ASSERT_EQ(ack.kind, Kind::ACK);
  EXPECT_EQ(ack.bytes.size(), 7u) << "ACK_R with cap byte";

  // An app retry while the message is on Alice's radio lands on the entry and is not sent again.
  size_t txt = p.net.count(Kind::TXT, p.bob.id);
  Node::AppSend r = p.bob.appSend(p.alice, "hello alice", 1, ts);
  EXPECT_TRUE(r.handled);
  EXPECT_FALSE(r.sent);
  EXPECT_EQ(r.est_timeout_ms, 0u);
  EXPECT_EQ(p.net.count(Kind::TXT, p.bob.id), txt);

  EXPECT_EQ(p.alice.appSync(), std::vector<std::string>{"hello alice"});
  p.net.pump();   // ACK_S
  EXPECT_EQ(statusesOf(p.bob, r.app_ack).back(), UserStatus::DELIVERED) << "reported with the app's latest ack";
  EXPECT_EQ(p.bob.confirms.size(), 1u);
  EXPECT_TRUE(p.alice.appSync().empty());
}

// #3518: the receiver ACKs exactly the messages it stored.
TEST(RdmNode, AckOnlyForStoredMessages) {
  Net net;
  Peer& bob = net.add();
  Peer& alice = net.add(20000);   // small flash: small inbox and a per-sender quota
  net.befriend(bob, alice);
  bob.boot();
  alice.boot();
  ASSERT_TRUE(alice.node->enabled());

  std::vector<std::string> sent;
  for (int i = 0; i < 12; i++) {
    sent.push_back("msg " + std::to_string(i));
    bob.appSend(alice, sent.back(), 0, net.epoch + 100 + i);
    net.pump();
  }
  size_t acks = net.count(Kind::ACK, alice.id, bob.id);
  EXPECT_LT(acks, sent.size()) << "quota reached";
  auto shown = alice.appSync();
  EXPECT_EQ(shown.size(), acks);
  for (size_t i = 0; i < shown.size(); i++) EXPECT_EQ(shown[i], sent[i]);
}

// G16: a lost ACK_R makes the sender resend; the register keeps it to one display.
TEST(RdmNode, LostAckIsResentAndShownOnce) {
  Pair p;
  int dropped = 0;
  p.net.drop = [&](const wire::Msg& m) { return m.kind == Kind::ACK && dropped++ == 0; };
  Node::AppSend a = p.bob.appSend(p.alice, "are you there");
  p.net.pump();
  EXPECT_TRUE(p.bob.confirms.empty());
  p.net.run(200 * 1000);
  EXPECT_EQ(statusesOf(p.bob, a.app_ack).back(), UserStatus::ON_RADIO);
  EXPECT_EQ(p.net.count(Kind::TXT, p.bob.id), 2u);
  EXPECT_EQ(p.alice.appSync(), std::vector<std::string>{"are you there"});
}

// A known-cap contact that was offline is probed with RECEIPT_QUERY; the answer UNKNOWN brings the DM.
TEST(RdmNode, ProbeAnswerIsRoutedAndDeliversAfterAliceReturns) {
  Pair p;
  p.bob.appSend(p.alice, "first");
  p.net.pump();
  ASSERT_EQ(p.bob.confirms.size(), 1u);

  p.alice.online = false;
  Node::AppSend a = p.bob.appSend(p.alice, "second");
  p.net.run(200 * 1000);
  EXPECT_GE(p.net.count(Kind::REQ, p.bob.id, p.alice.id, REQ_RECEIPT_QUERY), 1u);
  EXPECT_EQ(p.net.count(Kind::RESP), 0u);

  p.alice.online = true;
  p.net.run(35 * 60 * 1000);
  EXPECT_GE(p.net.count(Kind::RESP, p.alice.id, p.bob.id), 1u);
  EXPECT_EQ(statusesOf(p.bob, a.app_ack).back(), UserStatus::ON_RADIO);
  EXPECT_EQ(p.alice.appSync(), (std::vector<std::string>{"first", "second"}));
}

TEST(RdmNode, RuntimeOffBehavesLikeUpstream) {
  Pair p;
  p.alice.node->setEnabled(false);
  p.bob.node->setEnabled(false);
  EXPECT_FALSE(p.bob.node->enabled());

  Node::AppSend a = p.bob.appSend(p.alice, "plain");
  EXPECT_FALSE(a.handled);
  EXPECT_FALSE(a.sent);
  EXPECT_EQ(p.net.count(Kind::TXT, p.bob.id), 1u) << "the upstream DM";
  EXPECT_EQ(p.net.log.back().bytes.size(), 5u + 5u) << "no cap trailer";
  EXPECT_TRUE(p.bob.statuses.empty());
  p.net.pump();
  EXPECT_EQ(p.net.count(Kind::ACK), 0u) << "RdmChatMesh hands the DM to BaseChatMesh instead";

  uint8_t body[] = {REQ_RECEIPT_QUERY, 0}, reply[64];
  EXPECT_EQ(p.alice.node->onReceiptQuery(p.bob.pub, body, sizeof(body), reply), 0);
  EXPECT_EQ(p.bob.node->nextWakeupMillis(p.net.ms), UINT32_MAX);
  EXPECT_EQ(p.bob.node->hiddenPeerCount(), 0);
  OutEntry e[4];
  EXPECT_EQ(p.bob.node->listOutbox(e, 4), 0);
}

TEST(RdmNode, TooLongTextGoesUpstreamWithTooBigStatus) {
  Pair p;
  Node::AppSend a = p.bob.appSend(p.alice, std::string(MAX_TEXT + 1, 'x'));
  EXPECT_FALSE(a.handled);
  EXPECT_FALSE(a.sent);
  EXPECT_EQ(p.net.count(Kind::TXT, p.bob.id), 1u) << "the upstream DM";
  EXPECT_EQ(p.net.log.back().bytes.size(), 5u + MAX_TEXT + 1) << "no cap trailer";
  ASSERT_EQ(p.bob.statuses.size(), 1u);
  EXPECT_EQ(p.bob.statuses[0].s, UserStatus::TOO_BIG);
  EXPECT_EQ(p.bob.statuses[0].app_ack, a.app_ack);
  EXPECT_TRUE(p.bob.appSend(p.alice, std::string(MAX_TEXT, 'y')).handled);
}

TEST(RdmNode, MbxInfoGoesToFavouritesOnlyAndSetsTheSendersMailbox) {
  Net net;
  Peer& bob = net.add();
  Peer& alice = net.add();
  Peer& carol = net.add();
  Peer& mbx = net.add();
  mbx.on_req = [](Peer&, const wire::Msg&) {};
  net.befriend(bob, alice, true);
  net.befriend(carol, alice, false);
  bob.boot();
  alice.boot();
  carol.boot();
  const uint8_t k_owner[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
  ASSERT_TRUE(alice.node->setOwnMailbox(mbx.pub, k_owner));
  EXPECT_EQ(alice.node->hiddenPeerCount(), 1);

  bob.appSend(alice, "from a favourite");
  carol.appSend(alice, "from a stranger");
  net.run(10 * 1000);

  const wire::Msg* ctrl = nullptr;
  for (const auto& m : net.log)
    if (m.kind == Kind::TXT && m.from == alice.id && (m.bytes[4] >> 2) == TXT_TYPE_RDM_CTRL) {
      EXPECT_EQ(m.to, bob.id);
      ctrl = &m;
    }
  ASSERT_NE(ctrl, nullptr);
  uint8_t token[8];
  crypto::tokenB(token, k_owner, bob.pub);
  EXPECT_EQ(memcmp(&ctrl->bytes[7], mbx.pub, 32), 0);
  EXPECT_EQ(memcmp(&ctrl->bytes[39], token, 8), 0);

  ASSERT_EQ(bob.node->hiddenPeerCount(), 1);
  EXPECT_EQ(memcmp(bob.node->hiddenPeerPub(0), mbx.pub, 32), 0);
  EXPECT_EQ(carol.node->hiddenPeerCount(), 0);

  EXPECT_EQ(net.count(Kind::ACK, bob.id, alice.id), 1u) << "bob ACKs MBX_INFO";
  net.run(3 * 3600 * 1000);
  size_t ctrl_sent = 0;
  for (const auto& m : net.log)
    ctrl_sent += m.kind == Kind::TXT && m.from == alice.id && (m.bytes[4] >> 2) == TXT_TYPE_RDM_CTRL;
  EXPECT_EQ(ctrl_sent, 1u) << "the CTRL ACK ended the MBX_INFO entry";
}

// Copies from Alice's mailbox go into the inbox without a direct ACK; the outcome travels back as FETCH reports.
TEST(RdmNode, FetchReplyFeedsTheInboxAndReportsGoBackToTheMailbox) {
  Net net;
  Peer& bob = net.add();
  Peer& alice = net.add();
  Peer& mbx = net.add();
  net.befriend(bob, alice);
  bob.boot();
  alice.boot();

  uint8_t plain[200], inner[200];
  const uint32_t ts = net.epoch - 3600;
  size_t n = codec::buildTxtPlain(plain, ts, 0, FW_ATTEMPT_BASE, "kept for you", 12, true);
  size_t inner_len = sizeof(inner);
  ASSERT_TRUE(bob.encryptTxtPayload(alice.pub, plain, n, inner, inner_len));
  uint8_t hash[8];
  crypto::txtPacketHash(hash, inner, inner_len);

  std::vector<std::vector<Report>> fetches;
  std::vector<uint32_t> fetch_ms;
  mbx.on_req = [&](Peer& self, const wire::Msg& m) {
    uint8_t flags, rn;
    uint32_t store_id;
    Report r[MAX_BATCH];
    ASSERT_EQ(m.bytes[4], REQ_FETCH);
    ASSERT_TRUE(codec::parseFetch(&m.bytes[4], m.bytes.size() - 4, flags, store_id, r, rn));
    fetches.emplace_back(r, r + rn);
    fetch_ms.push_back(m.t_ms);
    uint8_t reply[4 + 200];
    memcpy(reply, m.bytes.data(), 4);
    size_t len = codec::buildFetchResp(reply + 4, 0, rn, fetches.size() == 1 ? inner : nullptr,
                                       fetches.size() == 1 ? (uint8_t)inner_len : 0);
    self.net.send(Kind::RESP, self.id, m.from, reply, 4 + len);
  };
  const uint8_t k_owner[16] = {9};
  ASSERT_TRUE(alice.node->setOwnMailbox(mbx.pub, k_owner));

  net.run(70 * 1000);
  ASSERT_GE(fetches.size(), 2u);
  EXPECT_TRUE(fetches[0].empty());
  ASSERT_EQ(fetches[1].size(), 1u);
  EXPECT_EQ(fetches[1][0].result, ReportResult::ON_RADIO);
  EXPECT_EQ(memcmp(fetches[1][0].pkt_hash, hash, 8), 0);
  uint8_t ack_r[4];
  crypto::ackR(ack_r, ts, FW_ATTEMPT_BASE & 3, "kept for you", 12, bob.pub);
  EXPECT_EQ(memcmp(fetches[1][0].ack, ack_r, 4), 0);
  EXPECT_GE(fetch_ms[1] - fetch_ms[0], RDM_MBX_REQ_GAP_S * 1000u);
  EXPECT_EQ(net.count(Kind::ACK, alice.id), 0u) << "a mailbox copy is never ACKed directly";

  EXPECT_EQ(alice.appSync(), std::vector<std::string>{"kept for you"});
  net.run(20 * 1000);
  ASSERT_GE(fetches.size(), 3u);
  ASSERT_EQ(fetches.back().size(), 1u);
  EXPECT_EQ(fetches.back()[0].result, ReportResult::SYNCED);
  EXPECT_EQ(net.count(Kind::ACK, alice.id), 0u) << "ACK_S of a copy goes via the mailbox";
}

TEST(RdmNode, ResponseWithUnknownTagIsNotRdm) {
  Pair p;
  uint8_t resp[] = {1, 2, 3, 4, 0};
  EXPECT_FALSE(p.bob.node->onResponse(p.alice.pub, resp, sizeof(resp)));
}

TEST(RdmNode, SlotCountsFollowFreeFlashAndStayStableOverReboots) {
  Net net;
  Peer& tiny = net.add(RDM_MIN_FREE_BYTES - 1);
  tiny.boot();
  EXPECT_FALSE(tiny.node->enabled());
  EXPECT_FALSE(tiny.node->appSend(tiny.pub, 1, 0, "x", 1).handled);

  Peer& small = net.add(64 * 1024);
  small.boot();
  ASSERT_TRUE(small.node->enabled());
  EXPECT_LE(small.io.usedBytes(), 32u * 1024);
  std::map<std::string, size_t> sizes;
  for (const auto& f : small.io.files) sizes[f.first] = f.second.size();
  EXPECT_LT(sizes["/rdm/inbox"], RecordFile::fileSize(188, RDM_INBOX_SLOTS_MAX, false));

  small.io.files["/other"] = std::vector<uint8_t>(20000, 0);   // other data grows: layout must not change
  small.reboot();
  ASSERT_TRUE(small.node->enabled());
  for (const auto& f : small.io.files)
    if (f.first != "/other") EXPECT_EQ(f.second.size(), sizes[f.first]) << f.first;
}

TEST(RdmNode, OutboxSurvivesRebootAndLateAckSStillDelivers) {
  Pair p;
  Node::AppSend a = p.bob.appSend(p.alice, "survive");
  p.net.pump();
  ASSERT_EQ(statusesOf(p.bob, a.app_ack).back(), UserStatus::ON_RADIO);

  p.bob.reboot();
  OutEntry e[4];
  ASSERT_EQ(p.bob.node->listOutbox(e, 4), 1);
  EXPECT_EQ(e[0].state, OutState::ON_RADIO);
  p.alice.appSync();
  p.net.pump();
  EXPECT_EQ(statusesOf(p.bob, a.app_ack).back(), UserStatus::DELIVERED);
}

// H4: jumping from wakeup to wakeup gives the same transmissions as 1 s polling; the wakeup is never late.
TEST(RdmNode, NextWakeupIsNeverLate) {
  auto scenario = [](bool jump) {
    Pair p;
    p.alice.online = false;
    p.bob.appSend(p.alice, "wake me");
    p.net.pump();
    const uint32_t end = p.net.ms + 3 * 3600 * 1000;
    while (p.net.ms < end) {
      uint32_t step = 1000;
      if (jump) {
        step = p.bob.node->nextWakeupMillis(p.net.ms);
        uint32_t w = p.alice.node->nextWakeupMillis(p.net.ms);
        if (w < step) step = w;
        if (step == 0) step = 1;
        if (step > end - p.net.ms) step = end - p.net.ms;
      }
      p.net.ms += step;
      p.bob.node->loop();
      p.net.pump();
    }
    std::vector<uint32_t> tx;
    for (const auto& m : p.net.log)
      if (m.from == p.bob.id) tx.push_back(m.t_ms);
    return tx;
  };
  auto polled = scenario(false);
  auto jumped = scenario(true);
  ASSERT_GE(polled.size(), 3u);
  ASSERT_EQ(polled.size(), jumped.size());
  for (size_t i = 0; i < polled.size(); i++) {
    EXPECT_LE(jumped[i], polled[i]) << "tx " << i;
    EXPECT_GE(jumped[i] + 1000, polled[i]) << "tx " << i;
  }
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
