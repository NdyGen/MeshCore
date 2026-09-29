// The two instruments the scenarios rely on, checked against independent references: SubprocessBackend against
// the real mbxd (line protocol of 06 par. 3.16, both directions) and the wire decoder against upstream traffic
// whose ACK is known from BaseChatMesh.

#include <gtest/gtest.h>

#include <ChatNode.h>
#include <RdmScenario.h>
#include <SubprocessBackend.h>
#include <helpers/rdm/RdmCrypto.h>

#include <stdlib.h>
#include <string.h>

#include <string>

using namespace sim;

namespace {

std::string tempDir() {
  const char* base = getenv("TMPDIR");
  std::string tmpl = std::string(base && *base ? base : "/tmp") + "/rdm_mbxd_XXXXXX";
  std::vector<char> buf(tmpl.begin(), tmpl.end());
  buf.push_back(0);
  return mkdtemp(buf.data()) ? std::string(buf.data()) : std::string();
}

void fill(uint8_t* p, size_t n, uint8_t seed) {
  for (size_t i = 0; i < n; i++) p[i] = (uint8_t)(seed + i * 7);
}

struct MbxdFixture : ::testing::Test {
  uint32_t now = 1790640000;
  std::string mbxd = SubprocessBackend::locateMbxd();
  std::string dir = tempDir();
  std::unique_ptr<SubprocessBackend> be;
  SubprocessBackend::Owner alice;
  uint8_t bob[32], owner4[4];

  void SetUp() override {
    ASSERT_FALSE(mbxd.empty()) << "mbxd.py not found (set RDM_MBXD or run from the repo root)";
    ASSERT_FALSE(dir.empty());
    fill(alice.pub, 32, 0x11);
    fill(alice.k_owner, 16, 0x51);
    fill(bob, 32, 0x77);
    memcpy(owner4, alice.pub, 4);
    be.reset(new SubprocessBackend(mbxd, dir + "/mbxd.db", [this] { return now; }));
    ASSERT_TRUE(be->ownerAdd(alice)) << be->lastError();
    ASSERT_TRUE(be->start()) << be->lastError();
    be->hello();
  }
  void TearDown() override {
    if (be) be->stop();
    if (!dir.empty()) system(("rm -rf '" + dir + "'").c_str());
  }
  rdm::BackendReply next() {
    rdm::BackendReply r;
    memset(&r, 0, sizeof(r));
    EXPECT_TRUE(be->poll(r)) << "no reply queued";
    return r;
  }
};

}

TEST_F(MbxdFixture, HelloGivesOwnerAclReadyAndThePiClock) {
  ASSERT_TRUE(be->ready());
  uint8_t pub[32], o4[4];
  bool is_owner = false;
  ASSERT_TRUE(be->pollAcl(pub, is_owner, o4));
  EXPECT_TRUE(is_owner);
  EXPECT_EQ(0, memcmp(pub, alice.pub, 32));
  EXPECT_EQ(0, memcmp(o4, owner4, 4));
  EXPECT_FALSE(be->pollAcl(pub, is_owner, o4));
  EXPECT_EQ(now, be->backendTime());
  now += 25;
  EXPECT_EQ(now, be->backendTime());   // extrapolated between mbx.time lines, like SerialPiBackend
}

TEST_F(MbxdFixture, RegisterStoreFetchReportAndStatRoundTrip) {
  uint8_t token[8], bad[8];
  rdm::crypto::tokenB(token, alice.k_owner, bob);
  memcpy(bad, token, 8);
  bad[0] ^= 1;

  ASSERT_TRUE(be->reg(1, bob, owner4, bad));
  rdm::BackendReply r = next();
  EXPECT_EQ(rdm::BackendReply::Kind::REG, r.kind);
  EXPECT_EQ(1u, r.id);
  EXPECT_EQ(rdm::MbxCode::NOT_AUTH, r.code);

  ASSERT_TRUE(be->reg(2, bob, owner4, token));
  r = next();
  EXPECT_EQ(rdm::MbxCode::OK, r.code);
  EXPECT_EQ(7, r.ttl_days);
  EXPECT_EQ(20, r.quota);

  uint8_t inner[100], hash[8];
  fill(inner, sizeof(inner), 3);
  rdm::crypto::txtPacketHash(hash, inner, sizeof(inner));
  ASSERT_TRUE(be->store(3, owner4, bob, hash, inner, sizeof(inner)));
  r = next();
  EXPECT_EQ(rdm::BackendReply::Kind::STORE, r.kind);
  EXPECT_EQ(rdm::MbxCode::OK, r.code);
  EXPECT_EQ(now + 7 * 86400, r.expires);
  EXPECT_EQ(1u, be->countReplies("store", 0x00));

  now += 3600;
  ASSERT_TRUE(be->fetch(4, alice.pub, 0, 0xA1B2C3D4, nullptr, 0));
  r = next();
  EXPECT_EQ(rdm::BackendReply::Kind::FETCH, r.kind);
  EXPECT_EQ(0, r.remaining);
  ASSERT_EQ(sizeof(inner), r.inner_len);
  EXPECT_EQ(0, memcmp(r.inner, inner, sizeof(inner)));   // bit for bit, base64 both ways

  rdm::Report rep;
  memcpy(rep.pkt_hash, hash, 8);
  rep.result = rdm::ReportResult::ON_RADIO;
  fill(rep.ack, 6, 0xA0);
  ASSERT_TRUE(be->fetch(5, alice.pub, rdm::FETCH_FLAG_NO_PAYLOAD, 0xA1B2C3D4, &rep, 1));
  r = next();
  EXPECT_EQ(1, r.reports_ok);
  EXPECT_EQ(0, r.inner_len);

  ASSERT_TRUE(be->stat(6, bob, &hash, 1));
  r = next();
  EXPECT_EQ(rdm::BackendReply::Kind::STAT, r.kind);
  ASSERT_EQ(1, r.n);
  EXPECT_EQ(rdm::MbxState::ON_RADIO, r.items[0].state);
  EXPECT_EQ(0, memcmp(r.items[0].ack, rep.ack, 6));

  rep.result = rdm::ReportResult::SYNCED;
  fill(rep.ack, 6, 0xC0);
  ASSERT_TRUE(be->fetch(7, alice.pub, 0, 0xA1B2C3D4, &rep, 1));
  next();
  ASSERT_TRUE(be->stat(8, bob, &hash, 1));
  r = next();
  EXPECT_EQ(rdm::MbxState::DELIVERED, r.items[0].state);
  EXPECT_EQ(0, memcmp(r.items[0].ack, rep.ack, 6));
  EXPECT_FALSE(be->poll(r));
}

TEST_F(MbxdFixture, PiClockFollowsTheSimulationForExpiry) {
  uint8_t token[8], inner[40], hash[8];
  rdm::crypto::tokenB(token, alice.k_owner, bob);
  fill(inner, sizeof(inner), 9);
  rdm::crypto::txtPacketHash(hash, inner, sizeof(inner));
  ASSERT_TRUE(be->reg(1, bob, owner4, token));
  next();
  ASSERT_TRUE(be->store(2, owner4, bob, hash, inner, sizeof(inner)));
  next();

  now += 7 * 86400 - 1;
  ASSERT_TRUE(be->stat(3, bob, &hash, 1));
  EXPECT_EQ(rdm::MbxState::STORED, next().items[0].state);
  now += 1;
  ASSERT_TRUE(be->stat(4, bob, &hash, 1));
  EXPECT_EQ(rdm::MbxState::EXPIRED, next().items[0].state);
}

TEST_F(MbxdFixture, RestartKeepsMessagesAndAnswersHelloAgain) {
  uint8_t token[8], inner[40], hash[8];
  rdm::crypto::tokenB(token, alice.k_owner, bob);
  fill(inner, sizeof(inner), 9);
  rdm::crypto::txtPacketHash(hash, inner, sizeof(inner));
  ASSERT_TRUE(be->reg(1, bob, owner4, token));
  next();
  ASSERT_TRUE(be->store(2, owner4, bob, hash, inner, sizeof(inner)));
  next();

  ASSERT_TRUE(be->restart()) << be->lastError();
  EXPECT_FALSE(be->ready());
  be->hello();
  EXPECT_TRUE(be->ready());
  ASSERT_TRUE(be->stat(3, bob, &hash, 1));
  EXPECT_EQ(rdm::MbxState::STORED, next().items[0].state);
}

// Decoder against upstream: bob's flood DM and alice's PATH carrying the ACK that BaseChatMesh computes.
TEST(WireLog, DecodesUpstreamDmAndItsAckViaRepeater) {
  Simulator s(7);
  auto& alice = s.add<ChatNode>("alice");
  auto& rep = s.add<RepeaterNode>("repeater");
  auto& bob = s.add<ChatNode>("bob");
  s.link(bob, rep);
  s.link(rep, alice);
  WireLog wires(s);
  wires.addNode(alice);
  wires.addNode(bob);
  wires.addNode(rep);
  alice.advert();
  s.run_for(5000);
  bob.advert();
  s.run_for(5000);

  const std::string text = "decode me";
  int id = bob.send_text(alice, text, 2);
  ASSERT_TRUE(s.run_until([&] { return bob.sent(id).status == ChatNode::Status::DELIVERED; }, 60000));
  s.run_for(5000);

  auto dms = wires.sent(bob, Kind::DM);
  ASSERT_EQ(1u, dms.size()) << wires.dump();
  EXPECT_EQ(alice.index(), dms[0].dest);
  EXPECT_EQ(text, dms[0].text);
  EXPECT_EQ(bob.sent(id).timestamp, dms[0].ts);
  EXPECT_EQ(2, dms[0].attempt);
  EXPECT_FALSE(dms[0].cap);
  EXPECT_TRUE(dms[0].flood);
  auto fwd = wires.select([&](const Wire& w) { return w.kind == Kind::DM && !w.original(); });
  ASSERT_EQ(1u, fwd.size());
  EXPECT_EQ(rep.index(), fwd[0].tx_node);
  EXPECT_EQ(bob.index(), fwd[0].origin);

  auto acks = wires.sent(alice, Kind::ACK);
  ASSERT_EQ(1u, acks.size()) << wires.dump();
  EXPECT_EQ(Kind::PATH, acks[0].kind);
  EXPECT_EQ(expectAckR(dms[0].ts, 2, text, bob.identity().pub_key), ackValue(acks[0].ack));
  EXPECT_EQ(bob.sent(id).expected_ack, ackValue(acks[0].ack));
}
