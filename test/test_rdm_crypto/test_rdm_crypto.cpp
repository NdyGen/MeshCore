#include <gtest/gtest.h>

#include <helpers/rdm/RdmCodec.h>
#include <helpers/rdm/RdmCrypto.h>

#include <Packet.h>
#include <Utils.h>

#include "ChatNode.h"

#include <string>
#include <vector>

using namespace rdm;

namespace {

const uint8_t PUB[32] = {0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15,
                         16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31};

std::vector<uint8_t> hex(const char* s) {
  std::vector<uint8_t> out;
  for (size_t i = 0; s[i] && s[i + 1]; i += 2) out.push_back((uint8_t)std::stoi(std::string(s + i, 2), nullptr, 16));
  return out;
}

std::string randomText(sim::SimRNG& rng, size_t len) {
  std::string t;
  for (size_t i = 0; i < len; i++) t.push_back((char)(0x20 + rng.next() % 95));
  return t;
}

}

// Reference values computed with Python hashlib/hmac (little-endian timestamp), see test comments.
TEST(RdmCrypto, KnownVectors) {
  uint8_t out[8];
  // sha256(pack('<I', 0x66AABBCC) + b'\x01' + b'hello' + pub)[:4]
  crypto::ackR(out, 0x66AABBCC, 1, "hello", 5, PUB);
  EXPECT_EQ(std::vector<uint8_t>(out, out + 4), hex("7bd7391e"));
  // sha256(b'RDMS' + pack('<I', ts) + b'\x00' + b'hello' + pub)[:6]
  crypto::ackS(out, 0x66AABBCC, 0, "hello", 5, PUB);
  EXPECT_EQ(std::vector<uint8_t>(out, out + 6), hex("58b2221c48f1"));
  // sha256(pack('<I', ts) + b'hello' + pub)[:4]
  crypto::key(out, 0x66AABBCC, "hello", 5, PUB);
  EXPECT_EQ(std::vector<uint8_t>(out, out + 4), hex("97608c93"));
}

TEST(RdmCrypto, TokenBMatchesPythonHmac) {
  uint8_t k_owner[16], pub[32], out[8];
  for (int i = 0; i < 16; i++) k_owner[i] = (uint8_t)i;
  for (int i = 0; i < 32; i++) pub[i] = (uint8_t)(0x20 + i);
  // hmac.new(bytes(range(16)), bytes(range(0x20, 0x40)), sha256).hexdigest()[:16]
  crypto::tokenB(out, k_owner, pub);
  EXPECT_EQ(std::vector<uint8_t>(out, out + 8), hex("21b8e892bb84c076"));

  memset(k_owner, 0xA5, sizeof(k_owner));
  memset(pub, 0x5A, sizeof(pub));
  crypto::tokenB(out, k_owner, pub);
  EXPECT_EQ(std::vector<uint8_t>(out, out + 8), hex("7baff5da3d30a8d3"));
}

TEST(RdmCrypto, TokenBDependsOnSenderPub) {
  uint8_t k_owner[16] = {1}, pub2[32], a[8], b[8];
  memcpy(pub2, PUB, 32);
  pub2[31] ^= 1;
  crypto::tokenB(a, k_owner, PUB);
  crypto::tokenB(b, k_owner, pub2);
  EXPECT_NE(0, memcmp(a, b, 8));
}

// K and ACK_S identify the message, not the attempt: equal for app attempts 0-3 and firmware attempts 252+c.
TEST(RdmCrypto, KeyAndAckSIndependentOfAttempt) {
  const char* text = "attempt independent";
  size_t n = strlen(text);
  uint8_t k0[4], s0[6];
  crypto::key(k0, 1790640000, text, n, PUB);
  crypto::ackS(s0, 1790640000, 0, text, n, PUB);

  std::vector<uint8_t> attempts = {0, 1, 2, 3, 252, 253, 254, 255, 248, 249};
  for (uint8_t attempt : attempts) {
    uint8_t plain[200];
    size_t len = codec::buildTxtPlain(plain, 1790640000, 0, attempt, text, n, true);
    ASSERT_GT(len, 0u);
    codec::TxtParsed p;
    ASSERT_TRUE(codec::parseTxtPlain(plain, len, p));
    uint8_t k[4], s[6];
    crypto::key(k, p.ts, p.text, p.text_len, PUB);
    crypto::ackS(s, p.ts, p.txt_type, p.text, p.text_len, PUB);
    EXPECT_EQ(0, memcmp(k, k0, 4)) << "attempt " << (int)attempt;
    EXPECT_EQ(0, memcmp(s, s0, 6)) << "attempt " << (int)attempt;
  }
}

// ACK_R depends on the class (attempt & 3) only, so firmware attempt 252 + c keeps the app's ACK.
TEST(RdmCrypto, AckRPerClass) {
  const char* text = "class";
  uint8_t r[4][4];
  for (int c = 0; c < 4; c++) crypto::ackR(r[c], 1790640000, (uint8_t)c, text, 5, PUB);
  for (int c = 0; c < 4; c++)
    for (int d = c + 1; d < 4; d++) EXPECT_NE(0, memcmp(r[c], r[d], 4)) << c << " vs " << d;
  for (int c = 0; c < 4; c++) {
    uint8_t fw[4];
    crypto::ackR(fw, 1790640000, (uint8_t)((FW_ATTEMPT_BASE + c) & 3), text, 5, PUB);
    EXPECT_EQ(0, memcmp(fw, r[c], 4));
  }
}

TEST(RdmCrypto, AckSDiffersFromAckRAndKey) {
  const char* text = "same inputs";
  uint8_t r[4], s[6], k[4];
  crypto::ackR(r, 1790640000, 0, text, strlen(text), PUB);
  crypto::ackS(s, 1790640000, 0, text, strlen(text), PUB);
  crypto::key(k, 1790640000, text, strlen(text), PUB);
  EXPECT_NE(0, memcmp(r, s, 4));
  EXPECT_NE(0, memcmp(k, s, 4));
  EXPECT_NE(0, memcmp(r, k, 4));
}

TEST(RdmCrypto, AckSCoversTxtType) {
  uint8_t plain[6], ctrl[6];
  crypto::ackS(plain, 1790640000, 0, "x", 1, PUB);
  crypto::ackS(ctrl, 1790640000, TXT_TYPE_RDM_CTRL, "x", 1, PUB);
  EXPECT_NE(0, memcmp(plain, ctrl, 6));
}

TEST(RdmCrypto, CtrlAckIsHashOfWholePlaintext) {
  codec::MbxInfo info{};
  info.sub = CTRL_MBX_INFO;
  info.version = CTRL_VERSION;
  memset(info.mbx_pub, 0x42, 32);
  memset(info.token, 0x17, 8);
  uint8_t plain[64];
  size_t len = codec::buildCtrlPlain(plain, 1790640000, info);
  ASSERT_EQ(len, 47u);

  uint8_t ack[4], expect[4];
  crypto::ctrlAck(ack, plain, len, PUB);
  mesh::Utils::sha256(expect, 4, plain, (int)len, PUB, 32);
  EXPECT_EQ(0, memcmp(ack, expect, 4));

  plain[46] ^= 1;
  crypto::ctrlAck(expect, plain, len, PUB);
  EXPECT_NE(0, memcmp(ack, expect, 4)) << "token is covered by the ACK";
}

// Real upstream code: 20 random DMs through BaseChatMesh::sendMessage (composeMsgPacket) on the simulator.
// ACK_R must equal the upstream expected_ack, buildTxtPlain without trailer must equal the upstream plaintext, and
// txtPacketHash must equal Packet::calculatePacketHash of the transmitted packet.
TEST(RdmCrypto, MatchesUpstreamComposeMsgPacket) {
  sim::Simulator s(11);
  auto& bob = s.add<sim::ChatNode>("bob");
  auto& alice = s.add<sim::ChatNode>("alice");
  bob.add_contact(alice);

  uint8_t secret[PUB_KEY_SIZE];
  bob.identity().calcSharedSecret(secret, alice.identity().pub_key);

  sim::SimRNG rng;
  rng.seed(20260929);
  for (int i = 0; i < 20; i++) {
    uint8_t attempt = (i % 5 == 4) ? (uint8_t)(FW_ATTEMPT_BASE + (i & 3)) : (uint8_t)(i & 3);
    size_t max_len = attempt > 3 ? MAX_TEXT_LEN - 2 : MAX_TEXT_LEN;
    std::string text = randomText(rng, 1 + rng.next() % max_len);
    uint32_t ts = 1790640000 + (uint32_t)(rng.next() % 1000000);

    size_t tx_before = s.tx_log().size();
    int id = bob.send_text(alice, text, attempt, ts);
    ASSERT_GE(id, 0);
    ASSERT_TRUE(s.run_until([&] { return s.tx_log().size() > tx_before; }, 10000));
    const sim::TxRecord& tx = s.tx_log()[tx_before];
    ASSERT_EQ(tx.payload_type, PAYLOAD_TYPE_TXT_MSG);

    uint8_t ack[4];
    crypto::ackR(ack, ts, attempt & 3, text.data(), text.size(), bob.identity().pub_key);
    uint32_t expected = bob.sent(id).expected_ack;
    EXPECT_EQ(0, memcmp(ack, &expected, 4)) << "message " << i;

    mesh::Packet pkt;
    ASSERT_TRUE(pkt.readFrom(tx.raw.data(), (uint8_t)tx.raw.size()));
    uint8_t h_upstream[MAX_HASH_SIZE], h_rdm[8];
    pkt.calculatePacketHash(h_upstream);
    crypto::txtPacketHash(h_rdm, pkt.payload, pkt.payload_len);
    EXPECT_EQ(0, memcmp(h_upstream, h_rdm, 8)) << "message " << i;

    uint8_t plain[MAX_PACKET_PAYLOAD];
    int plain_len = mesh::Utils::MACThenDecrypt(secret, plain, &pkt.payload[2 * PATH_HASH_SIZE],
                                                pkt.payload_len - 2 * PATH_HASH_SIZE);
    ASSERT_GT(plain_len, 0);
    uint8_t mine[MAX_PACKET_PAYLOAD] = {};
    size_t mine_len = codec::buildTxtPlain(mine, ts, 0, attempt, text.data(), text.size(), false);
    ASSERT_GT(mine_len, 0u);
    ASSERT_GE((size_t)plain_len, mine_len);
    EXPECT_EQ(0, memcmp(plain, mine, mine_len)) << "message " << i;
    for (int j = (int)mine_len; j < plain_len; j++) EXPECT_EQ(plain[j], 0) << "zero padding only";
  }
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
