#include <gtest/gtest.h>

#include <ChatNode.h>
#include <RdmSimMailbox.h>
#include <helpers/rdm/RdmCodec.h>
#include <helpers/rdm/RdmCrypto.h>

#include <MailboxMesh.h>

#include <cstring>
#include <map>
#include <stdexcept>
#include <vector>

using namespace sim;
using namespace rdm;

namespace {

// A client on the real BaseChatMesh that only speaks the mailbox requests; responses are logged by tag.
class MbxClient : public SimNode {
  class Impl : public BaseChatMesh {
    MbxClient& _node;

  public:
    Impl(MbxClient& n, mesh::Radio& radio, mesh::MillisecondClock& ms, mesh::RNG& rng, mesh::RTCClock& rtc,
         mesh::PacketManager& mgr, mesh::MeshTables& tables)
        : BaseChatMesh(radio, ms, rng, rtc, mgr, tables), _node(n) {}

  protected:
    void onDiscoveredContact(ContactInfo&, bool, uint8_t, const uint8_t*) override {}
    ContactInfo* processAck(const uint8_t*) override { return nullptr; }
    void onContactPathUpdated(const ContactInfo&) override {}
    void onMessageRecv(const ContactInfo&, mesh::Packet*, uint32_t, const char*) override {}
    void onCommandDataRecv(const ContactInfo&, mesh::Packet*, uint32_t, const char*) override {}
    void onCLICommandRecv(const ContactInfo&, mesh::Packet*, uint32_t, const char*, char*) override {}
    void onSignedMessageRecv(const ContactInfo&, mesh::Packet*, uint32_t, const uint8_t*, const char*) override {}
    uint32_t calcFloodTimeoutMillisFor(uint32_t airtime) const override { return 500 + 16 * airtime; }
    uint32_t calcDirectTimeoutMillisFor(uint32_t airtime, uint8_t) const override { return 500 + 6 * airtime; }
    void onSendTimeout() override {}
    void onChannelMessageRecv(const mesh::GroupChannel&, mesh::Packet*, uint32_t, const char*) override {}
    uint8_t onContactRequest(const ContactInfo&, uint32_t, const uint8_t*, uint8_t, uint8_t*) override { return 0; }
    void onContactResponse(const ContactInfo&, const uint8_t* data, uint8_t len) override {
      if (len < 4) return;
      uint32_t tag;
      memcpy(&tag, data, 4);
      _node.responses[tag].assign(data + 4, data + len);
    }
  };

  Impl* _impl = nullptr;

public:
  std::map<uint32_t, std::vector<uint8_t>> responses;

  MbxClient(Simulator& sim, std::string name, int index) : SimNode(sim, std::move(name), index) {}

  BaseChatMesh& chat() { return *_impl; }

  void addContact(SimNode& other) {
    ContactInfo c;
    memset(&c, 0, sizeof(c));
    c.id = other.identity();
    c.type = ADV_TYPE_ROOM;
    c.out_path_len = OUT_PATH_UNKNOWN;
    chat().addContact(c);
  }
  ContactInfo* contact(SimNode& other) { return chat().lookupContactByPubKey(other.identity().pub_key, PUB_KEY_SIZE); }

  uint32_t anon(SimNode& to, const uint8_t* plain_without_ts, uint8_t len) {
    Simulator::OnNode ctx(sim(), *this);
    uint32_t tag, est;
    EXPECT_NE(chat().sendAnonReq(*contact(to), plain_without_ts, len, tag, est), MSG_SEND_FAILED);
    return tag;
  }
  uint32_t req(SimNode& to, const uint8_t* body, uint8_t len) {
    Simulator::OnNode ctx(sim(), *this);
    uint32_t tag, est;
    EXPECT_NE(chat().sendRequest(*contact(to), body, len, tag, est), MSG_SEND_FAILED);
    return tag;
  }
  bool got(uint32_t tag) const { return responses.count(tag) > 0; }

protected:
  mesh::Mesh* createMesh() override {
    _impl = new Impl(*this, radio(), millisClock(), rng(), rtc(), packetManager(), tables());
    return _impl;
  }
  void loopMesh() override { _impl->loop(); }
};

class MailboxSimTest : public ::testing::Test {
protected:
  Simulator s{7};
  RdmSimMailbox& m = s.add<RdmSimMailbox>("mbx");
  RepeaterNode& r = s.add<RepeaterNode>("rep");
  MbxClient& bob = s.add<MbxClient>("bob");
  MbxClient& alice = s.add<MbxClient>("alice");
  uint8_t k_owner[16];

  void SetUp() override {
    for (int i = 0; i < 16; i++) k_owner[i] = (uint8_t)(0x21 * i);
    s.link(bob, r);
    s.link(alice, r);
    s.link(m, r);
    ASSERT_TRUE(m.backend().addOwner(alice.identity().pub_key, k_owner));
    m.reboot();   // a new owner needs an mbxd restart, and opening the port resets the radio
    bob.addContact(m);
    alice.addContact(m);
  }

  uint32_t registerBob() {
    uint8_t token[8], plain[32];
    crypto::tokenB(token, k_owner, bob.identity().pub_key);
    size_t n = codec::buildRegReq(plain, 0, alice.identity().pub_key, token);
    return bob.anon(m, plain + 4, (uint8_t)(n - 4));   // BaseChatMesh puts the timestamp (tag) in front
  }
};

}

TEST_F(MailboxSimTest, RegisterDepositAndFetchOverTheAir) {
  // Registration by flood: answered with a PATH return, so Bob learns the way to M.
  uint32_t tag = registerBob();
  ASSERT_TRUE(s.run_until([&] { return bob.got(tag); }, 60000));
  MbxCode st; uint8_t ttl, quota; uint32_t time_m;
  const auto& reg = bob.responses[tag];
  ASSERT_TRUE(codec::parseRegResp(reg.data(), reg.size(), st, ttl, quota, time_m));
  EXPECT_EQ(st, MbxCode::OK);
  EXPECT_LE(time_m, s.wall_epoch()) << "tijd_M follows the Pi clock";
  EXPECT_GE(time_m + 5, s.wall_epoch());
  EXPECT_TRUE(m.mailbox().knowsPeer(bob.identity().pub_key));
  s.run_for(3000);
  ASSERT_NE(bob.contact(m)->out_path_len, OUT_PATH_UNKNOWN);

  // Deposit, direct.
  uint8_t inner[60];
  for (int i = 0; i < 60; i++) inner[i] = (uint8_t)(i * 3 + 1);
  uint8_t body[200];
  size_t n = codec::buildDeposit(body, alice.identity().pub_key, inner, sizeof(inner));
  s.run_for(6000);
  tag = bob.req(m, body, (uint8_t)n);
  ASSERT_TRUE(s.run_until([&] { return bob.got(tag); }, 30000));
  uint8_t hash[8]; uint32_t ttl_s;
  ASSERT_TRUE(codec::parseDepositResp(bob.responses[tag].data(), bob.responses[tag].size(), st, hash, ttl_s));
  EXPECT_EQ(st, MbxCode::OK);
  EXPECT_EQ(ttl_s, 7u * 86400);
  uint8_t expect_hash[8];
  crypto::txtPacketHash(expect_hash, inner, sizeof(inner));
  EXPECT_EQ(0, memcmp(hash, expect_hash, 8));

  // Alice's first FETCH floods: PATH return without the copy.
  n = codec::buildFetch(body, 0, 0x1234, nullptr, 0);
  tag = alice.req(m, body, (uint8_t)n);
  ASSERT_TRUE(s.run_until([&] { return alice.got(tag); }, 60000));
  uint8_t remaining, ok, inner_len;
  const uint8_t* got;
  ASSERT_TRUE(codec::parseFetchResp(alice.responses[tag].data(), alice.responses[tag].size(), remaining, ok, got,
                                    inner_len));
  EXPECT_EQ(remaining, 1);
  EXPECT_EQ(inner_len, 0);
  s.run_for(6000);
  ASSERT_NE(alice.contact(m)->out_path_len, OUT_PATH_UNKNOWN);

  // Second FETCH goes direct and carries the copy bit for bit.
  tag = alice.req(m, body, (uint8_t)n);
  ASSERT_TRUE(s.run_until([&] { return alice.got(tag); }, 30000));
  ASSERT_TRUE(codec::parseFetchResp(alice.responses[tag].data(), alice.responses[tag].size(), remaining, ok, got,
                                    inner_len));
  EXPECT_EQ(remaining, 0);
  ASSERT_EQ(inner_len, sizeof(inner));
  EXPECT_EQ(0, memcmp(got, inner, sizeof(inner)));
}

TEST_F(MailboxSimTest, AclSurvivesMailboxRebootSoDepositorsAreStillDecryptable) {
  uint32_t tag = registerBob();
  ASSERT_TRUE(s.run_until([&] { return bob.got(tag); }, 60000));
  m.reboot();
  s.run_for(100);
  EXPECT_TRUE(m.mailbox().knowsPeer(bob.identity().pub_key)) << "filled from the Pi after HELLO";
  EXPECT_TRUE(m.mailbox().knowsPeer(alice.identity().pub_key));
}

TEST_F(MailboxSimTest, FloodAdvertAfterBootThenEveryTwelveHoursWithG17Jitter) {
  size_t before = s.count_tx(m.index(), PAYLOAD_TYPE_ADVERT);
  s.run_for(MBX_BOOT_ADVERT_DELAY_MS + 2000);
  EXPECT_EQ(s.count_tx(m.index(), PAYLOAD_TYPE_ADVERT), before + 1);

  s.set_fast_forward(3600 * 1000);   // must land on the advert timer, not up to a step late
  s.run_for(6 * (MBX_FLOOD_ADVERT_INTERVAL_S + 60) * 1000UL);
  std::vector<uint64_t> at;
  for (auto& t : s.tx_log())
    if (t.from == m.index() && t.payload_type == PAYLOAD_TYPE_ADVERT) at.push_back(t.t_ms);
  ASSERT_GE(at.size(), 7u);
  // G17: I + U[0, min(I / 10, 60 s)], drawn again for every step, so mailboxes and clients do not stay in step.
  std::vector<uint64_t> gaps;
  for (size_t i = 1; i < at.size(); i++) {
    uint64_t gap = at[i] - at[i - 1];
    EXPECT_GE(gap, MBX_FLOOD_ADVERT_INTERVAL_S * 1000ULL) << "advert " << i;
    EXPECT_LE(gap, (MBX_FLOOD_ADVERT_INTERVAL_S + 60 + 1) * 1000ULL) << "advert " << i;
    gaps.push_back(gap / 1000);
  }
  bool varies = false;
  for (auto g : gaps) varies |= g != gaps[0];
  EXPECT_TRUE(varies) << "jitter is drawn per interval";
}

TEST_F(MailboxSimTest, PiAbsentDepositGetsNoStorageAfterThreeSeconds) {
  uint32_t tag = registerBob();
  ASSERT_TRUE(s.run_until([&] { return bob.got(tag); }, 60000));
  s.run_for(6000);
  m.backend().setMode(MemMailboxBackend::Mode::NO_REPLY);
  uint8_t inner[40] = {9}, body[100];
  size_t n = codec::buildDeposit(body, alice.identity().pub_key, inner, sizeof(inner));
  tag = bob.req(m, body, (uint8_t)n);
  uint64_t sent_at = s.now();
  ASSERT_TRUE(s.run_until([&] { return bob.got(tag); }, 30000));
  EXPECT_GE(s.now() - sent_at, 3000u);
  MbxCode st; uint8_t hash[8]; uint32_t ttl_s;
  ASSERT_TRUE(codec::parseDepositResp(bob.responses[tag].data(), bob.responses[tag].size(), st, hash, ttl_s));
  EXPECT_EQ(st, MbxCode::NO_STORAGE);
}

namespace {

const TxRecord* lastFrom(Simulator& s, int from, uint8_t type) {
  const TxRecord* last = nullptr;
  for (auto& t : s.tx_log())
    if (t.from == from && t.payload_type == type) last = &t;
  return last;
}

// Transport code a scoped flood must carry: HMAC over payload type and payload with the region key.
uint16_t expectedCode(const TxRecord& t, const char* region) {
  TransportKey key;
  TransportKeyStore store;
  std::string name = std::string("#") + region;
  store.getAutoKeyFor(0, name.c_str(), key);
  mesh::Packet pkt;
  EXPECT_TRUE(pkt.readFrom(t.raw.data(), (uint8_t)t.raw.size()));
  return key.calcTransportCode(&pkt);
}

}

TEST_F(MailboxSimTest, DirectRegistrationGetsDirectAnswer) {
  uint32_t tag = registerBob();
  ASSERT_TRUE(s.run_until([&] { return bob.got(tag); }, 60000));
  const TxRecord* first = lastFrom(s, m.index(), PAYLOAD_TYPE_PATH);
  ASSERT_NE(first, nullptr) << "flood registration answered with a PATH return";
  s.run_for(6000);
  ASSERT_NE(bob.contact(m)->out_path_len, OUT_PATH_UNKNOWN);
  size_t paths = s.count_tx(m.index(), PAYLOAD_TYPE_PATH);

  tag = registerBob();   // Bob knows the path now: the ANON_REQ goes direct
  ASSERT_TRUE(s.run_until([&] { return bob.got(tag); }, 30000));
  EXPECT_EQ(s.count_tx(m.index(), PAYLOAD_TYPE_PATH), paths) << "no PATH return for a direct request";
  const TxRecord* resp = lastFrom(s, m.index(), PAYLOAD_TYPE_RESPONSE);
  ASSERT_NE(resp, nullptr);
  EXPECT_FALSE(resp->flood);
}

TEST_F(MailboxSimTest, FloodsAreUnscopedByDefault) {
  s.run_for(MBX_BOOT_ADVERT_DELAY_MS + 2000);
  const TxRecord* advert = lastFrom(s, m.index(), PAYLOAD_TYPE_ADVERT);
  ASSERT_NE(advert, nullptr);
  EXPECT_EQ(advert->header & PH_ROUTE_MASK, ROUTE_TYPE_FLOOD);
}

TEST_F(MailboxSimTest, FloodScopeAddsTheRegionTransportCode) {
  m.mailbox().setFloodScope("nl");
  uint32_t tag = registerBob();
  ASSERT_TRUE(s.run_until([&] { return bob.got(tag); }, 60000)) << "the repeater forwards scoped floods";
  const TxRecord* path = lastFrom(s, m.index(), PAYLOAD_TYPE_PATH);
  ASSERT_NE(path, nullptr);
  EXPECT_EQ(path->header & PH_ROUTE_MASK, ROUTE_TYPE_TRANSPORT_FLOOD);
  uint16_t code = (uint16_t)(path->raw[1] | (path->raw[2] << 8));
  EXPECT_EQ(code, expectedCode(*path, "nl"));

  s.run_for(MBX_BOOT_ADVERT_DELAY_MS);
  const TxRecord* advert = lastFrom(s, m.index(), PAYLOAD_TYPE_ADVERT);
  ASSERT_NE(advert, nullptr);
  EXPECT_EQ(advert->header & PH_ROUTE_MASK, ROUTE_TYPE_TRANSPORT_FLOOD);
  EXPECT_EQ((uint16_t)(advert->raw[1] | (advert->raw[2] << 8)), expectedCode(*advert, "nl"));

  m.mailbox().setFloodScope(nullptr);
  m.advert(true);
  s.run_for(2000);
  EXPECT_EQ(lastFrom(s, m.index(), PAYLOAD_TYPE_ADVERT)->header & PH_ROUTE_MASK, ROUTE_TYPE_FLOOD);
}

TEST(RdmSimMailboxInjected, UsesTheInjectedBackendAndItsHooks) {
  Simulator s{11};
  MemMailboxBackend pi;   // stands in for any external backend (WP9: SubprocessBackend with real mbxd)
  pi.setTimeSource([&s]() { return s.wall_epoch(); });
  int boots = 0, busy_asked = 0;
  auto& m = s.add<RdmSimMailbox>("mbx", pi, [&]() { boots++; pi.hello(); }, [&]() { busy_asked++; return false; });
  auto& r = s.add<RepeaterNode>("rep");
  auto& bob = s.add<MbxClient>("bob");
  auto& alice = s.add<MbxClient>("alice");
  s.link(bob, r);
  s.link(alice, r);
  s.link(m, r);
  EXPECT_EQ(boots, 1) << "on_boot runs at every boot, like HELLO from the radio";
  EXPECT_FALSE(m.ownsBackend());
  EXPECT_THROW(m.backend(), std::logic_error);

  uint8_t k_owner[16] = {1, 2, 3};
  ASSERT_TRUE(pi.addOwner(alice.identity().pub_key, k_owner));
  m.reboot();
  EXPECT_EQ(boots, 2);
  s.run_for(100);
  EXPECT_TRUE(m.mailbox().knowsPeer(alice.identity().pub_key)) << "ACL from the injected backend";

  bob.addContact(m);
  uint8_t token[8], plain[32];
  crypto::tokenB(token, k_owner, bob.identity().pub_key);
  size_t n = codec::buildRegReq(plain, 0, alice.identity().pub_key, token);
  uint32_t tag = bob.anon(m, plain + 4, (uint8_t)(n - 4));
  ASSERT_TRUE(s.run_until([&] { return bob.got(tag); }, 60000));
  EXPECT_EQ(bob.responses[tag][0], (uint8_t)MbxCode::OK);
  EXPECT_GE(pi.requestCount(), 1u);

  s.set_fast_forward(60000);
  s.run_for(120000);
  EXPECT_GT(busy_asked, 0) << "fast-forward consults the busy hook";
}

TEST(RdmSimMailboxOwned, OwnsItsMemBackendByDefault) {
  Simulator s{12};
  auto& m = s.add<RdmSimMailbox>("mbx");
  EXPECT_TRUE(m.ownsBackend());
  EXPECT_NO_THROW(m.backend());
}
