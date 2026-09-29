#include <gtest/gtest.h>

#include <SerialPiBackend.h>

#include <string>

using namespace rdm;

namespace {

class FakeStream : public Stream {
public:
  std::string in, out;
  size_t pos = 0;
  int available() override { return (int)(in.size() - pos); }
  int read() override { return pos < in.size() ? (uint8_t)in[pos++] : -1; }
  int peek() override { return pos < in.size() ? (uint8_t)in[pos] : -1; }
  size_t write(uint8_t b) override { out += (char)b; return 1; }
  void feed(const std::string& s) { in += s; }
  std::string takeOut() { std::string s = out; out.clear(); return s; }
};

class FakeMillis : public mesh::MillisecondClock {
public:
  unsigned long ms = 1000;
  unsigned long getMillis() override { return ms; }
};

std::string rep(const char* two_hex, int n) {
  std::string s;
  for (int i = 0; i < n; i++) s += two_hex;
  return s;
}

class SerialPiBackendTest : public ::testing::Test {
protected:
  FakeStream io;
  FakeMillis clock;
  SerialPiBackend be{io, clock};
  uint8_t owner[4] = {0x53, 0x5b, 0x31, 0xdb};
  uint8_t bob[32], hash[8];

  void SetUp() override {
    memset(bob, 0x11, 32);
    memset(hash, 0xab, 8);
    be.begin("1.0-rdm");
  }
};

}

TEST_F(SerialPiBackendTest, HelloAtBeginAndReadyOnlyAfterMbxReady) {
  EXPECT_EQ(io.takeOut(), "@MBX HELLO 1.0-rdm\n");
  EXPECT_FALSE(be.ready());
  io.feed("mbx.acl " + rep("a0", 32) + " o a0a0a0a0\n");
  EXPECT_FALSE(be.ready());
  io.feed("mbx.ready\n");
  EXPECT_TRUE(be.ready());
}

TEST_F(SerialPiBackendTest, HelloRepeatsEveryThirtySecondsUntilReady) {
  io.takeOut();
  clock.ms += 29999;
  be.ready();
  EXPECT_EQ(io.takeOut(), "");
  clock.ms += 1;
  be.ready();
  EXPECT_EQ(io.takeOut(), "@MBX HELLO 1.0-rdm\n");
  io.feed("mbx.ready\n");
  be.ready();
  clock.ms += 60000;
  be.ready();
  EXPECT_EQ(io.takeOut(), "");
}

TEST_F(SerialPiBackendTest, RequestLinesAsRecorded) {
  io.takeOut();
  const uint8_t payload[5] = {'h', 'e', 'l', 'l', 'o'};
  ASSERT_TRUE(be.store(7, owner, bob, hash, payload, 5));
  EXPECT_EQ(io.takeOut(), "@MBX STORE 7 535b31db " + rep("11", 32) + " abababababababab aGVsbG8=\n");

  uint8_t token[8] = {0xdd, 0x05, 0x2b, 0x2f, 0x71, 0x2e, 0x47, 0x82};
  ASSERT_TRUE(be.reg(4294967295u, bob, owner, token));
  EXPECT_EQ(io.takeOut(), "@MBX REG 4294967295 " + rep("11", 32) + " 535b31db dd052b2f712e4782\n");

  ASSERT_TRUE(be.fetch(8, bob, 0x02, 0x0000abcd, nullptr, 0));
  EXPECT_EQ(io.takeOut(), "@MBX FETCH 8 " + rep("11", 32) + " 02 0000abcd -\n");

  Report r[2];
  memset(r[0].pkt_hash, 0x01, 8);
  r[0].result = ReportResult::ON_RADIO;
  memcpy(r[0].ack, "\xaa\xbb\xcc\xdd\x00\x00", 6);
  memset(r[1].pkt_hash, 0x02, 8);
  r[1].result = ReportResult::INBOX_FULL;
  memset(r[1].ack, 0, 6);
  ASSERT_TRUE(be.fetch(9, bob, 0, 0xffffffffu, r, 2));
  EXPECT_EQ(io.takeOut(), "@MBX FETCH 9 " + rep("11", 32) + " 00 ffffffff " + rep("01", 8) + ":0:aabbccdd0000," +
                              rep("02", 8) + ":4:000000000000\n");

  uint8_t hashes[2][8];
  memset(hashes[0], 0x0f, 8);
  memset(hashes[1], 0xf0, 8);
  ASSERT_TRUE(be.stat(10, bob, hashes, 2));
  EXPECT_EQ(io.takeOut(), "@MBX STAT 10 " + rep("11", 32) + " " + rep("0f", 8) + "," + rep("f0", 8) + "\n");
}

TEST_F(SerialPiBackendTest, LongestLinesFitAndInvalidBatchesAreRefused) {
  io.takeOut();
  uint8_t payload[MAX_INNER_PAYLOAD];
  memset(payload, 0xff, sizeof(payload));
  ASSERT_TRUE(be.store(4294967295u, owner, bob, hash, payload, MAX_INNER_PAYLOAD));
  std::string line = io.takeOut();
  EXPECT_EQ(line.back(), '\n');
  EXPECT_EQ(line.size(), 11u + 10 + 1 + 8 + 1 + 64 + 1 + 16 + 1 + 220 + 1);
  EXPECT_FALSE(be.store(1, owner, bob, hash, payload, 0));
  Report r[MAX_BATCH + 1] = {};
  ASSERT_TRUE(be.fetch(4294967295u, bob, 0xff, 0xffffffffu, r, MAX_BATCH));
  EXPECT_LE(io.takeOut().size(), 400u);
  EXPECT_FALSE(be.fetch(1, bob, 0, 0, r, MAX_BATCH + 1));
  uint8_t hashes[MAX_BATCH + 1][8] = {};
  EXPECT_FALSE(be.stat(1, bob, hashes, 0));
  EXPECT_FALSE(be.stat(1, bob, hashes, MAX_BATCH + 1));
  EXPECT_EQ(io.takeOut(), "");
}

TEST_F(SerialPiBackendTest, NeverSendsTheTestTimeLine) {
  io.feed("mbx.ready\nmbx.time 1790000000\n");
  be.ready();
  EXPECT_EQ(io.takeOut().find("TIME"), std::string::npos);
}

// Replies below are mbxd output (examples/mailbox_server/mbxd, test_mbxd.py and the conformance vectors).
TEST_F(SerialPiBackendTest, ParsesStoreRegFetchStatReplies) {
  io.feed("mbx.store 7 00 1790604810\n"
          "mbx.reg 8 10 0 0\n"
          "mbx.fetch 9 00 2 1 aGVsbG8=\n"
          "mbx.fetch 10 00 0 0 -\n"
          "mbx.stat 11 00 2:aabbccdd0000,0:000000000000,3:" + rep("5a", 6) + "\n");
  BackendReply r;
  ASSERT_TRUE(be.poll(r));
  EXPECT_EQ(r.kind, BackendReply::Kind::STORE);
  EXPECT_EQ(r.id, 7u);
  EXPECT_EQ(r.code, MbxCode::OK);
  EXPECT_EQ(r.expires, 1790604810u);

  ASSERT_TRUE(be.poll(r));
  EXPECT_EQ(r.kind, BackendReply::Kind::REG);
  EXPECT_EQ(r.id, 8u);
  EXPECT_EQ(r.code, MbxCode::NOT_AUTH);
  EXPECT_EQ(r.ttl_days, 0);

  ASSERT_TRUE(be.poll(r));
  EXPECT_EQ(r.kind, BackendReply::Kind::FETCH);
  EXPECT_EQ(r.remaining, 2);
  EXPECT_EQ(r.reports_ok, 1);
  ASSERT_EQ(r.inner_len, 5);
  EXPECT_EQ(0, memcmp(r.inner, "hello", 5));

  ASSERT_TRUE(be.poll(r));
  EXPECT_EQ(r.id, 10u);
  EXPECT_EQ(r.inner_len, 0);

  ASSERT_TRUE(be.poll(r));
  EXPECT_EQ(r.kind, BackendReply::Kind::STAT);
  ASSERT_EQ(r.n, 3);
  EXPECT_EQ(r.items[0].state, MbxState::ON_RADIO);
  EXPECT_EQ(0, memcmp(r.items[0].ack, "\xaa\xbb\xcc\xdd\x00\x00", 6));
  EXPECT_EQ(r.items[1].state, MbxState::UNKNOWN);
  EXPECT_EQ(r.items[2].state, MbxState::DELIVERED);
  EXPECT_FALSE(be.poll(r));
}

TEST_F(SerialPiBackendTest, ParsesRegOkAndStatWithoutItems) {
  io.feed("mbx.reg 1 00 7 20\r\nmbx.stat 2 13 -\n");
  BackendReply r;
  ASSERT_TRUE(be.poll(r));
  EXPECT_EQ(r.code, MbxCode::OK);
  EXPECT_EQ(r.ttl_days, 7);
  EXPECT_EQ(r.quota, 20);
  ASSERT_TRUE(be.poll(r));
  EXPECT_EQ(r.kind, BackendReply::Kind::STAT);
  EXPECT_EQ(r.code, MbxCode::NO_STORAGE);
  EXPECT_EQ(r.n, 0);
}

TEST_F(SerialPiBackendTest, AclLinesInOrder) {
  io.feed("mbx.acl " + rep("a0", 32) + " o a0a0a0a0\n"
          "mbx.acl " + rep("11", 32) + " d a0a0a0a0\n"
          "mbx.ready\n");
  uint8_t pub[32], o4[4];
  bool is_owner;
  ASSERT_TRUE(be.pollAcl(pub, is_owner, o4));
  EXPECT_TRUE(is_owner);
  EXPECT_EQ(pub[0], 0xa0);
  EXPECT_EQ(o4[3], 0xa0);
  ASSERT_TRUE(be.pollAcl(pub, is_owner, o4));
  EXPECT_FALSE(is_owner);
  EXPECT_EQ(pub[31], 0x11);
  EXPECT_FALSE(be.pollAcl(pub, is_owner, o4));
  EXPECT_TRUE(be.ready());
}

TEST_F(SerialPiBackendTest, SixtyFourAclLinesInOneBurstAllArrive) {
  std::string burst;
  for (int i = 0; i < 64; i++) {
    char pub[65];
    for (int j = 0; j < 32; j++) snprintf(pub + 2 * j, 3, "%02x", (i + j) & 0xff);
    burst += std::string("mbx.acl ") + pub + " d 01020304\n";
  }
  io.feed(burst + "mbx.ready\n");
  uint8_t pub[32], o4[4];
  bool is_owner;
  int n = 0;
  while (be.pollAcl(pub, is_owner, o4)) {
    EXPECT_EQ(pub[0], n);
    n++;
  }
  EXPECT_EQ(n, 64);
  EXPECT_TRUE(be.ready());
}

TEST_F(SerialPiBackendTest, DebugNoiseMalformedAndOverlongLinesAreSkipped) {
  io.feed("12:00:00 - 29/9/2026 U RAW: 0102\n"
          "mbx.store x 00 5\n"
          "mbx.store 1 0 5\n"
          "mbx.store 1 00\n"
          "mbx.fetch 2 00 0 0 aGVsbG8\n"
          "mbx.fetch 2 00 0 0 a*VsbG8=\n"
          "mbx.stat 3 00 9:000000000000\n"
          "mbx.stat 3 00 2:0000\n"
          "mbx.acl " + rep("a0", 31) + " o a0a0a0a0\n"
          "mbx.acl " + rep("a0", 32) + " x a0a0a0a0\n"
          "mbx.time -5\n"
          "mbx." + std::string(500, 'z') + "\n"
          "mbx.store 4 01 1790000000\n");
  BackendReply r;
  ASSERT_TRUE(be.poll(r));
  EXPECT_EQ(r.id, 4u);
  EXPECT_EQ(r.code, MbxCode::ALREADY_STORED);
  EXPECT_FALSE(be.poll(r));
  uint8_t pub[32], o4[4];
  bool is_owner;
  EXPECT_FALSE(be.pollAcl(pub, is_owner, o4));
  EXPECT_EQ(be.backendTime(), 0u);
}

TEST_F(SerialPiBackendTest, FetchPayloadAboveInnerLimitIsRejected) {
  std::string b64;
  for (int i = 0; i < 56; i++) b64 += "////";   // 168 bytes
  io.feed("mbx.fetch 5 00 0 0 " + b64 + "\n");
  BackendReply r;
  EXPECT_FALSE(be.poll(r));
}

TEST_F(SerialPiBackendTest, LinesSplitAcrossReads) {
  io.feed("mbx.sto");
  BackendReply r;
  EXPECT_FALSE(be.poll(r));
  io.feed("re 3 00 17");
  EXPECT_FALSE(be.poll(r));
  io.feed("90000000\n");
  ASSERT_TRUE(be.poll(r));
  EXPECT_EQ(r.expires, 1790000000u);
}

TEST_F(SerialPiBackendTest, BackendTimeFollowsMbxTimeWithMillis) {
  EXPECT_EQ(be.backendTime(), 0u);
  io.feed("mbx.time 1790000000\n");
  EXPECT_EQ(be.backendTime(), 1790000000u);
  clock.ms += 3999;
  EXPECT_EQ(be.backendTime(), 1790000003u);
  io.feed("mbx.time 1790000600\n");
  EXPECT_EQ(be.backendTime(), 1790000600u) << "a new mbx.time replaces the extrapolation";
  clock.ms += 1000;
  EXPECT_EQ(be.backendTime(), 1790000601u);
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
