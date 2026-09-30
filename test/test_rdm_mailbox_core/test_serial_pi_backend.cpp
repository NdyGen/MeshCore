#include <gtest/gtest.h>

#include <SerialPiBackend.h>
#include <TestUtil.h>

#include "../test_rdm_mailbox_conformance/MiniJson.h"

#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

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
  EXPECT_EQ(io.takeOut(), "@MBX HELLO 2 1.0-rdm\n");
  EXPECT_FALSE(be.ready());
  io.feed("mbx.acl " + rep("a0", 32) + " o a0a0a0a0\n");
  EXPECT_FALSE(be.ready());
  io.feed("mbx.ready 2\n");
  EXPECT_TRUE(be.ready());
}

TEST_F(SerialPiBackendTest, HelloRepeatsEveryThirtySecondsUntilReady) {
  io.takeOut();
  clock.ms += 29999;
  be.ready();
  EXPECT_EQ(io.takeOut(), "");
  clock.ms += 1;
  be.ready();
  EXPECT_EQ(io.takeOut(), "@MBX HELLO 2 1.0-rdm\n");
  io.feed("mbx.ready 2\n");
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
  io.feed("mbx.ready 2\nmbx.time 1790000000\n");
  be.ready();
  EXPECT_EQ(io.takeOut().find("TIME"), std::string::npos);
}

// Replies below are daemon output (meshcore-mailboxd and the conformance vectors).
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
          "mbx.ready 2\n");
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
  io.feed(burst + "mbx.ready 2\n");
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

// ---- session protocol v2 (06 par. 3.16) ----

TEST_F(SerialPiBackendTest, HelloQueryWhileReadyIsAnsweredAndKeepsReady) {
  io.takeOut();
  io.feed("mbx.acl " + rep("a0", 32) + " o a0a0a0a0\nmbx.ready 2\nmbx.time 1790000000\n");
  ASSERT_TRUE(be.ready());
  io.feed("mbx.hello?\n");
  EXPECT_TRUE(be.ready()) << "a daemon restart does not drop readiness";
  EXPECT_EQ(io.takeOut(), "@MBX HELLO 2 1.0-rdm\n");
  clock.ms += 29999;
  EXPECT_TRUE(be.ready());
  EXPECT_EQ(io.takeOut(), "");
  clock.ms += 1;
  EXPECT_TRUE(be.ready());
  EXPECT_EQ(io.takeOut(), "@MBX HELLO 2 1.0-rdm\n") << "the answer to mbx.hello? is repeated until some mbx.ready";
  io.feed("mbx.ready 2\n");
  EXPECT_TRUE(be.ready());
  clock.ms += 60000;
  EXPECT_TRUE(be.ready());
  EXPECT_EQ(io.takeOut(), "");
}

TEST_F(SerialPiBackendTest, SecondAclSeriesIsQueuedAgainAndReadyStays) {
  io.takeOut();
  io.feed("mbx.acl " + rep("a0", 32) + " o a0a0a0a0\nmbx.ready 2\nmbx.time 1790000000\n");
  uint8_t pub[32], o4[4];
  bool is_owner;
  ASSERT_TRUE(be.pollAcl(pub, is_owner, o4));
  EXPECT_FALSE(be.pollAcl(pub, is_owner, o4));
  ASSERT_TRUE(be.ready());

  // owner-add in another process: the same owner again plus the new one, then mbx.ready 2, no mbx.time
  io.feed("mbx.acl " + rep("a0", 32) + " o a0a0a0a0\nmbx.acl " + rep("b1", 32) + " o b1b1b1b1\nmbx.ready 2\n");
  ASSERT_TRUE(be.pollAcl(pub, is_owner, o4));
  EXPECT_EQ(pub[0], 0xa0);
  ASSERT_TRUE(be.pollAcl(pub, is_owner, o4));
  EXPECT_EQ(pub[0], 0xb1);
  EXPECT_TRUE(is_owner);
  EXPECT_FALSE(be.pollAcl(pub, is_owner, o4));
  EXPECT_TRUE(be.ready());
  EXPECT_EQ(be.daemonProto(), 2u);
  EXPECT_EQ(io.takeOut(), "") << "an ACL push needs no answer";
  clock.ms += 5;
  EXPECT_EQ(be.backendTime(), 1790000000u);
}

TEST_F(SerialPiBackendTest, ReadyWithAnotherVersionIsAMismatch) {
  io.takeOut();
  EXPECT_EQ(be.daemonProto(), 0u);
  io.feed("mbx.ready 3\n");
  EXPECT_FALSE(be.ready());
  EXPECT_EQ(be.daemonProto(), 3u);
  clock.ms += 60000;
  EXPECT_FALSE(be.ready());
  EXPECT_EQ(io.takeOut(), "") << "no HELLO retry after a mismatch: the radio waits for mbx.hello?";
  io.feed("mbx.hello?\n");
  EXPECT_FALSE(be.ready());
  EXPECT_EQ(io.takeOut(), "@MBX HELLO 2 1.0-rdm\n");
  io.feed("mbx.ready 2\n");
  EXPECT_TRUE(be.ready());
  EXPECT_EQ(be.daemonProto(), 2u);
}

TEST_F(SerialPiBackendTest, BareReadyIsProtocolOneAndAnUpgradedDaemonCanTakeOver) {
  io.takeOut();
  io.feed("mbx.ready\n");
  EXPECT_FALSE(be.ready());
  EXPECT_EQ(be.daemonProto(), 1u);
  clock.ms += 60000;
  EXPECT_FALSE(be.ready());
  EXPECT_EQ(io.takeOut(), "");
  io.feed("mbx.ready 2\n");
  EXPECT_TRUE(be.ready());
  io.feed("mbx.hello?\nmbx.ready 3\n");
  EXPECT_FALSE(be.ready()) << "a daemon that now speaks another version takes readiness away";
  EXPECT_EQ(be.daemonProto(), 3u);
  EXPECT_EQ(io.takeOut(), "@MBX HELLO 2 1.0-rdm\n");
}

TEST_F(SerialPiBackendTest, MalformedReadyAndHelloLinesAreIgnored) {
  io.takeOut();
  io.feed("mbx.hello?x\nmbx.hello? \nmbx.hello\nmbx.ready x\nmbx.ready 2 2\nmbx.ready  2\nmbx.ready -2\nmbx.readyx\n");
  EXPECT_FALSE(be.ready());
  EXPECT_EQ(be.daemonProto(), 0u);
  EXPECT_EQ(io.takeOut(), "");
  BackendReply r;
  EXPECT_FALSE(be.poll(r));
}

// ---- test/rdm_vectors/mbxd_session.json, radio side ----

namespace {

using minijson::Value;

std::string dirOf(const std::string& path) {
  size_t p = path.find_last_of('/');
  return p == std::string::npos ? std::string(".") : path.substr(0, p);
}

Value loadSessionVectors() {
  const std::string candidates[] = {
    dirOf(__FILE__) + "/../rdm_vectors/mbxd_session.json",
    "test/rdm_vectors/mbxd_session.json",
  };
  for (auto& path : candidates) {
    std::ifstream f(path);
    if (!f) continue;
    std::stringstream ss;
    ss << f.rdbuf();
    return minijson::parse(ss.str());
  }
  throw std::runtime_error("mbxd_session.json not found");
}

const Value& sessionVectors() {
  static Value v = loadSessionVectors();
  return v;
}

bool playsRadio(const Value& c) {
  if (!c.has("players")) return true;
  const Value& p = c["players"];
  for (size_t i = 0; i < p.size(); i++)
    if (p[i].asStr() == "radio") return true;
  return false;
}

std::vector<std::string> radioCaseNames() {
  std::vector<std::string> names;
  const Value& cases = sessionVectors()["cases"];
  for (size_t i = 0; i < cases.size(); i++)
    if (playsRadio(cases[i])) names.push_back(cases[i]["name"].asStr());
  return names;
}

const Value& sessionCase(const std::string& name) {
  const Value& cases = sessionVectors()["cases"];
  for (size_t i = 0; i < cases.size(); i++)
    if (cases[i]["name"].asStr() == name) return cases[i];
  throw std::runtime_error("no case " + name);
}

std::vector<std::string> splitLines(const std::string& s) {
  std::vector<std::string> out;
  size_t start = 0;
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == '\n') {
      out.push_back(s.substr(start, i - start));
      start = i + 1;
    }
  }
  if (start < s.size()) out.push_back(s.substr(start) + "<no newline>");
  return out;
}

std::vector<std::string> strings(const Value& arr) {
  std::vector<std::string> out;
  for (size_t i = 0; i < arr.size(); i++) out.push_back(arr[i].asStr());
  return out;
}

// The radio of one case: off until radio_boot, then a fresh backend on a fresh stream.
struct RadioPlayer {
  FakeStream io;
  FakeMillis clock;
  std::unique_ptr<SerialPiBackend> be;

  void pump() {
    if (be) be->ready();
  }

  void step(const Value& st) {
    if (st.has("from")) {
      if (st["from"].asStr() == "radio") {
        pump();
        EXPECT_EQ(splitLines(io.takeOut()), strings(st["lines"]));
      } else if (be) {
        for (const std::string& l : strings(st["lines"])) io.feed(l + "\n");
        pump();
      }
      return;
    }
    const std::string& op = st["op"].asStr();
    if (op == "radio_boot") {
      io = FakeStream();
      be.reset(new SerialPiBackend(io, clock));
      static std::string fw;
      fw = st["args"]["fw"].asStr();
      be->begin(fw.c_str());
    } else if (op == "radio_ms") {
      ASSERT_TRUE(be);
      clock.ms += (unsigned long)st["args"]["ms"].asInt();
      pump();
    } else if (op == "radio_reg") {
      ASSERT_TRUE(be);
      const Value& a = st["args"];
      uint8_t sender[32], owner[4], token[8];
      ASSERT_TRUE(fromHex(a["sender"].asStr(), sender, 32));
      ASSERT_TRUE(fromHex(a["owner"].asStr(), owner, 4));
      ASSERT_TRUE(fromHex(a["token"].asStr(), token, 8));
      ASSERT_TRUE(be->reg((uint32_t)a["id"].asInt(), sender, owner, token));
    } else if (op == "assert_radio") {
      ASSERT_TRUE(be);
      if (st.has("ready")) EXPECT_EQ(be->ready(), st["ready"].b);
      if (st.has("daemon_proto")) EXPECT_EQ(be->daemonProto(), (uint32_t)st["daemon_proto"].asInt());
      if (st.has("time")) EXPECT_EQ(be->backendTime(), (uint32_t)st["time"].asInt());
      if (st.has("acl")) {
        std::vector<std::string> got, want;
        uint8_t pub[32], o4[4];
        bool is_owner;
        while (be->pollAcl(pub, is_owner, o4)) got.push_back(toHexLower(pub, 32) + (is_owner ? " o " : " d ") + toHexLower(o4, 4));
        const Value& acl = st["acl"];
        for (size_t i = 0; i < acl.size(); i++)
          want.push_back(acl[i][0].asStr() + " " + acl[i][1].asStr() + " " + acl[i][2].asStr());
        EXPECT_EQ(got, want);
      }
      if (st.has("replies")) {
        const Value& replies = st["replies"];
        for (size_t i = 0; i < replies.size(); i++) {
          const Value& want = replies[i];
          BackendReply r;
          ASSERT_TRUE(be->poll(r)) << "reply " << i << " missing";
          const std::string& kind = want["kind"].asStr();
          EXPECT_EQ((int)r.kind, kind == "store" ? (int)BackendReply::Kind::STORE : kind == "reg" ? (int)BackendReply::Kind::REG
                                 : kind == "fetch" ? (int)BackendReply::Kind::FETCH : (int)BackendReply::Kind::STAT);
          EXPECT_EQ(r.id, (uint32_t)want["id"].asInt());
          EXPECT_EQ((int)r.code, (int)want["code"].asInt());
          if (want.has("ttl_days")) EXPECT_EQ(r.ttl_days, (int)want["ttl_days"].asInt());
          if (want.has("quota")) EXPECT_EQ(r.quota, (int)want["quota"].asInt());
        }
        BackendReply extra;
        EXPECT_FALSE(be->poll(extra)) << "more replies than expected";
      }
    }
    // pi_start, owner_add, deny, undeny: daemon-side events, nothing happens on the radio
  }
};

}

class SessionVectors : public ::testing::TestWithParam<std::string> {};

TEST_P(SessionVectors, RadioSide) {
  const Value& c = sessionCase(GetParam());
  RadioPlayer radio;
  const Value& steps = c["steps"];
  for (size_t i = 0; i < steps.size(); i++) {
    SCOPED_TRACE("step " + std::to_string(i) + " (" + (steps[i].has("op") ? steps[i]["op"].asStr() : "lines from " + steps[i]["from"].asStr()) + ")");
    radio.step(steps[i]);
    if (HasFatalFailure()) return;
  }
  radio.pump();
  EXPECT_EQ(radio.io.takeOut(), "") << "the radio sent lines the vector does not expect";
}

INSTANTIATE_TEST_SUITE_P(Vectors, SessionVectors, ::testing::ValuesIn(radioCaseNames()),
                         [](const ::testing::TestParamInfo<std::string>& info) { return info.param; });

TEST(SessionVectorsFile, CoverTheSessionRules) {
  const Value& cases = sessionVectors()["cases"];
  bool hello_query = false, ready_with_version = false, bare_ready = false, acl_push = false, v1_hello = false;
  size_t radio_cases = 0;
  for (size_t i = 0; i < cases.size(); i++) {
    radio_cases += playsRadio(cases[i]);
    const Value& steps = cases[i]["steps"];
    for (size_t k = 0; k < steps.size(); k++) {
      if (steps[k].has("op")) {
        acl_push |= steps[k]["op"].asStr() == "owner_add" && k + 1 < steps.size() && steps[k + 1].has("from");
        continue;
      }
      for (const std::string& l : strings(steps[k]["lines"])) {
        hello_query |= l == "mbx.hello?";
        ready_with_version |= l == "mbx.ready 2";
        bare_ready |= l == "mbx.ready";
        v1_hello |= l == "@MBX HELLO" || l.compare(0, 11, "@MBX HELLO ") == 0 && l.size() > 11 && (l[11] < '0' || l[11] > '9');
      }
    }
  }
  EXPECT_TRUE(hello_query);
  EXPECT_TRUE(ready_with_version);
  EXPECT_TRUE(bare_ready);
  EXPECT_TRUE(acl_push);
  EXPECT_TRUE(v1_hello);
  EXPECT_GE(radio_cases, 7u);
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
