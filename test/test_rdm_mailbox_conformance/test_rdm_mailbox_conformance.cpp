#include <gtest/gtest.h>

#include <MemMailboxBackend.h>

#include "MiniJson.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>

using namespace rdm;
using minijson::Value;

namespace {

std::string dirOf(const std::string& path) {
  size_t p = path.find_last_of('/');
  return p == std::string::npos ? std::string(".") : path.substr(0, p);
}

Value loadVectors() {
  const std::string candidates[] = {
    dirOf(__FILE__) + "/../rdm_vectors/mailbox_conformance.json",
    "test/rdm_vectors/mailbox_conformance.json",
  };
  for (auto& path : candidates) {
    std::ifstream f(path);
    if (!f) continue;
    std::stringstream ss;
    ss << f.rdbuf();
    return minijson::parse(ss.str());
  }
  throw std::runtime_error("mailbox_conformance.json not found");
}

const Value& vectors() {
  static Value v = loadVectors();
  return v;
}

std::vector<uint8_t> hex(const std::string& s) {
  if (s.size() % 2) throw std::runtime_error("odd hex length: " + s);
  std::vector<uint8_t> out(s.size() / 2);
  for (size_t i = 0; i < out.size(); i++) out[i] = (uint8_t)strtoul(s.substr(2 * i, 2).c_str(), nullptr, 16);
  return out;
}

std::vector<uint8_t> hexN(const Value& v, size_t n) {
  auto b = hex(v.asStr());
  if (b.size() != n) throw std::runtime_error("expected " + std::to_string(n) + " bytes: " + v.asStr());
  return b;
}

std::string toHex(const uint8_t* p, size_t n) {
  static const char* digits = "0123456789abcdef";
  std::string s;
  for (size_t i = 0; i < n; i++) { s += digits[p[i] >> 4]; s += digits[p[i] & 15]; }
  return s;
}

BackendReply onlyReply(MemMailboxBackend& be, BackendReply::Kind kind, uint32_t id) {
  BackendReply r;
  EXPECT_TRUE(be.poll(r)) << "no reply";
  EXPECT_EQ((int)r.kind, (int)kind);
  EXPECT_EQ(r.id, id);
  BackendReply extra;
  EXPECT_FALSE(be.poll(extra)) << "more than one reply";
  return r;
}

void runStep(MemMailboxBackend& be, uint32_t id, const Value& step) {
  be.setTime((uint32_t)step["t"].asInt());
  const std::string& op = step["op"].asStr();
  const Value& a = step["args"];
  if (op == "owner_add") {
    ASSERT_TRUE(be.addOwner(hexN(a["pub"], 32).data(), hexN(a["k_owner"], 16).data(), (uint8_t)a["ttl_days"].asInt(),
                            (uint8_t)a["sync_days"].asInt(), (uint8_t)a["quota"].asInt()));
  } else if (op == "deny") {
    ASSERT_TRUE(be.deny(hexN(a["owner"], 4).data(), hexN(a["pub"], 32).data()));
  } else if (op == "advance") {
    be.advance();
  } else if (op == "reg") {
    const Value& e = step["expect"];
    ASSERT_TRUE(be.reg(id, hexN(a["sender"], 32).data(), hexN(a["owner"], 4).data(), hexN(a["token"], 8).data()));
    BackendReply r = onlyReply(be, BackendReply::Kind::REG, id);
    EXPECT_EQ((int)r.code, e["code"].asInt());
    EXPECT_EQ(r.ttl_days, e["ttl_days"].asInt());
    EXPECT_EQ(r.quota, e["quota"].asInt());
  } else if (op == "store") {
    const Value& e = step["expect"];
    auto payload = hex(a["payload"].asStr());
    ASSERT_LE(payload.size(), 255u);
    ASSERT_TRUE(be.store(id, hexN(a["owner"], 4).data(), hexN(a["sender"], 32).data(), hexN(a["hash"], 8).data(),
                         payload.data(), (uint8_t)payload.size()));
    BackendReply r = onlyReply(be, BackendReply::Kind::STORE, id);
    EXPECT_EQ((int)r.code, e["code"].asInt());
    EXPECT_EQ(r.expires, (uint32_t)e["expires"].asInt());
  } else if (op == "fetch") {
    const Value& e = step["expect"];
    const Value& reps = a["reports"];
    Report reports[MAX_BATCH];
    ASSERT_LE(reps.size(), MAX_BATCH);
    for (size_t i = 0; i < reps.size(); i++) {
      memcpy(reports[i].pkt_hash, hexN(reps[i]["hash"], 8).data(), 8);
      reports[i].result = (ReportResult)reps[i]["result"].asInt();
      memcpy(reports[i].ack, hexN(reps[i]["ack"], 6).data(), 6);
    }
    auto sid = hexN(a["store_id"], 4);
    uint32_t store_id = ((uint32_t)sid[0] << 24) | ((uint32_t)sid[1] << 16) | ((uint32_t)sid[2] << 8) | sid[3];
    ASSERT_TRUE(be.fetch(id, hexN(a["client"], 32).data(), (uint8_t)a["flags"].asInt(), store_id, reports,
                         (uint8_t)reps.size()));
    BackendReply r = onlyReply(be, BackendReply::Kind::FETCH, id);
    EXPECT_EQ((int)r.code, e["code"].asInt());
    EXPECT_EQ(r.remaining, e["remaining"].asInt());
    EXPECT_EQ(r.reports_ok, e["reports_ok"].asInt());
    std::string got = r.inner_len ? toHex(r.inner, r.inner_len) : "null";
    EXPECT_EQ(got, e["payload"].isNull() ? std::string("null") : e["payload"].asStr());
  } else if (op == "stat") {
    const Value& e = step["expect"];
    const Value& hs = a["hashes"];
    uint8_t hashes[MAX_BATCH][8];
    ASSERT_LE(hs.size(), MAX_BATCH);
    for (size_t i = 0; i < hs.size(); i++) memcpy(hashes[i], hexN(hs[i], 8).data(), 8);
    ASSERT_TRUE(be.stat(id, hexN(a["sender"], 32).data(), hashes, (uint8_t)hs.size()));
    BackendReply r = onlyReply(be, BackendReply::Kind::STAT, id);
    EXPECT_EQ((int)r.code, e["code"].asInt());
    const Value& items = e["items"];
    ASSERT_EQ(r.n, items.size());
    for (size_t i = 0; i < items.size(); i++) {
      EXPECT_EQ((int)r.items[i].state, items[i]["state"].asInt()) << "item " << i;
      EXPECT_EQ(toHex(r.items[i].ack, 6), items[i]["ack"].asStr()) << "item " << i;
    }
  } else {
    FAIL() << "unknown op " << op;
  }
}

std::vector<std::string> caseNames() {
  std::vector<std::string> names;
  const Value& cases = vectors()["cases"];
  for (size_t i = 0; i < cases.size(); i++) names.push_back(cases[i]["name"].asStr());
  return names;
}

const Value& caseByName(const std::string& name) {
  const Value& cases = vectors()["cases"];
  for (size_t i = 0; i < cases.size(); i++)
    if (cases[i]["name"].asStr() == name) return cases[i];
  throw std::runtime_error("no case " + name);
}

}

class Conformance : public ::testing::TestWithParam<std::string> {};

TEST_P(Conformance, Case) {
  const Value& c = caseByName(GetParam());
  MemMailboxBackend be;
  const Value& steps = c["steps"];
  for (size_t i = 0; i < steps.size(); i++) {
    SCOPED_TRACE("step " + std::to_string(i) + " (" + steps[i]["op"].asStr() + " at t=" +
                 std::to_string(steps[i]["t"].asInt()) + ")");
    runStep(be, (uint32_t)i, steps[i]);
    if (HasFatalFailure()) return;
  }
}

INSTANTIATE_TEST_SUITE_P(Vectors, Conformance, ::testing::ValuesIn(caseNames()),
                         [](const ::testing::TestParamInfo<std::string>& info) { return info.param; });

TEST(ConformanceVectors, CoverEveryOpAndCode) {
  bool seen_op[7] = {};
  const char* ops[7] = {"store", "reg", "fetch", "stat", "owner_add", "deny", "advance"};
  std::vector<int> codes;
  const Value& cases = vectors()["cases"];
  for (size_t c = 0; c < cases.size(); c++) {
    const Value& steps = cases[c]["steps"];
    for (size_t i = 0; i < steps.size(); i++) {
      for (int k = 0; k < 7; k++) if (steps[i]["op"].asStr() == ops[k]) seen_op[k] = true;
      if (steps[i].has("expect")) codes.push_back((int)steps[i]["expect"]["code"].asInt());
    }
  }
  for (int k = 0; k < 7; k++) EXPECT_TRUE(seen_op[k]) << ops[k];
  for (MbxCode code : {MbxCode::OK, MbxCode::ALREADY_STORED, MbxCode::NOT_AUTH, MbxCode::UNKNOWN_OWNER,
                       MbxCode::QUOTA, MbxCode::TOO_BIG}) {
    EXPECT_NE(std::find(codes.begin(), codes.end(), (int)code), codes.end()) << "code " << (int)code;
  }
}

// ---- MemMailboxBackend specifics (not part of mbxd) ------------------------------------------------------

namespace {

struct Keys {
  uint8_t owner[32], k_owner[16], bob[32], carol[32];
  Keys() {
    for (int i = 0; i < 32; i++) {
      owner[i] = (uint8_t)(0xA0 + i);
      bob[i] = (uint8_t)(0x10 + i);
      carol[i] = (uint8_t)(0x30 + i);
    }
    for (int i = 0; i < 16; i++) k_owner[i] = (uint8_t)(0x55 ^ i);
  }
};

// HMAC-SHA256(k_owner, sender)[0:8] for the Keys above, computed with Python's hmac module.
const char* BOB_TOKEN = "eb39dff6e298af99";
const char* CAROL_TOKEN = "5a2839eab963ac3f";

void registerBob(MemMailboxBackend& be, Keys& k) {
  auto tok = hex(BOB_TOKEN);
  ASSERT_TRUE(be.addOwner(k.owner, k.k_owner));
  ASSERT_TRUE(be.reg(1, k.bob, k.owner, tok.data()));
  BackendReply r;
  ASSERT_TRUE(be.poll(r));
  ASSERT_EQ(r.code, MbxCode::OK) << "token vector does not match the HMAC implementation";
}

}

TEST(MemMailboxBackend, NoReplyModeDropsRequests) {
  Keys k;
  MemMailboxBackend be;
  be.setTime(1000);
  registerBob(be, k);
  be.setMode(MemMailboxBackend::Mode::NO_REPLY);
  uint8_t hash[8] = {1, 2, 3, 4, 5, 6, 7, 8}, payload[40] = {};
  EXPECT_TRUE(be.store(2, k.owner, k.bob, hash, payload, sizeof(payload)));
  BackendReply r;
  EXPECT_FALSE(be.poll(r));
  EXPECT_EQ(be.messageCount(), 0u) << "Pi absent: nothing is stored";
  EXPECT_EQ(be.requestCount(), 2u);

  be.setMode(MemMailboxBackend::Mode::NORMAL);
  EXPECT_TRUE(be.store(3, k.owner, k.bob, hash, payload, sizeof(payload)));
  ASSERT_TRUE(be.poll(r));
  EXPECT_EQ(r.code, MbxCode::OK);
  EXPECT_EQ(r.expires, 1000u + 7 * 86400);
}

TEST(MemMailboxBackend, WithholdModeNeverHandsOutCopies) {
  Keys k;
  MemMailboxBackend be;
  registerBob(be, k);
  uint8_t hash[8] = {9}, payload[40] = {};
  be.store(2, k.owner, k.bob, hash, payload, sizeof(payload));
  BackendReply r;
  be.poll(r);
  be.setMode(MemMailboxBackend::Mode::WITHHOLD);
  ASSERT_TRUE(be.fetch(3, k.owner, 0, 1, nullptr, 0));
  ASSERT_TRUE(be.poll(r));
  EXPECT_EQ(r.code, MbxCode::OK);
  EXPECT_EQ(r.inner_len, 0);
  EXPECT_EQ(r.remaining, 0);
  MemMailboxBackend::MsgState st;
  ASSERT_TRUE(be.messageState(hash, st));
  EXPECT_EQ(st, MemMailboxBackend::MsgState::STORED);
}

TEST(MemMailboxBackend, RejectsOversizedBatches) {
  Keys k;
  MemMailboxBackend be;
  registerBob(be, k);
  Report reports[MAX_BATCH + 1] = {};
  uint8_t hashes[MAX_BATCH + 1][8] = {};
  EXPECT_FALSE(be.fetch(2, k.owner, 0, 1, reports, MAX_BATCH + 1));
  EXPECT_FALSE(be.stat(3, k.bob, hashes, MAX_BATCH + 1));
  EXPECT_FALSE(be.stat(4, k.bob, hashes, 0));
  BackendReply r;
  EXPECT_FALSE(be.poll(r));
}

TEST(MemMailboxBackend, HelloQueuesAclOwnersFirst) {
  Keys k;
  MemMailboxBackend be;
  be.setReady(false);
  be.setTime(100);
  registerBob(be, k);
  uint8_t pub[32], owner4[4];
  bool is_owner;
  EXPECT_FALSE(be.pollAcl(pub, is_owner, owner4)) << "nothing before HELLO";
  EXPECT_FALSE(be.ready());

  be.hello();
  EXPECT_TRUE(be.ready());
  ASSERT_TRUE(be.pollAcl(pub, is_owner, owner4));
  EXPECT_TRUE(is_owner);
  EXPECT_EQ(0, memcmp(pub, k.owner, 32));
  EXPECT_EQ(0, memcmp(owner4, k.owner, 4));
  ASSERT_TRUE(be.pollAcl(pub, is_owner, owner4));
  EXPECT_FALSE(is_owner);
  EXPECT_EQ(0, memcmp(pub, k.bob, 32));
  EXPECT_EQ(0, memcmp(owner4, k.owner, 4));
  EXPECT_FALSE(be.pollAcl(pub, is_owner, owner4));
}

TEST(MemMailboxBackend, AclDepositorsMostRecentFirstOnePerPubkey) {
  Keys k;
  MemMailboxBackend be;
  uint8_t owner2[32] = {0xEE}, k2[16] = {};
  be.setTime(100);
  registerBob(be, k);
  ASSERT_TRUE(be.addOwner(owner2, k2));
  be.setTime(200);
  ASSERT_TRUE(be.reg(2, k.carol, k.owner, hex(CAROL_TOKEN).data()));
  be.setTime(300);
  ASSERT_TRUE(be.reg(3, k.bob, k.owner, hex(BOB_TOKEN).data()));
  BackendReply r;
  while (be.poll(r)) EXPECT_EQ(r.code, MbxCode::OK);

  be.hello();
  uint8_t pub[32], owner4[4];
  bool is_owner;
  ASSERT_TRUE(be.pollAcl(pub, is_owner, owner4));
  EXPECT_TRUE(is_owner && memcmp(pub, k.owner, 32) == 0);
  ASSERT_TRUE(be.pollAcl(pub, is_owner, owner4));
  EXPECT_TRUE(is_owner && memcmp(pub, owner2, 32) == 0);
  ASSERT_TRUE(be.pollAcl(pub, is_owner, owner4));
  EXPECT_TRUE(!is_owner && memcmp(pub, k.bob, 32) == 0) << "bob registered again last";
  ASSERT_TRUE(be.pollAcl(pub, is_owner, owner4));
  EXPECT_TRUE(!is_owner && memcmp(pub, k.carol, 32) == 0);
  EXPECT_FALSE(be.pollAcl(pub, is_owner, owner4));

  ASSERT_TRUE(be.deny(k.owner, k.carol));
  be.hello();
  int n = 0;
  while (be.pollAcl(pub, is_owner, owner4)) {
    EXPECT_NE(0, memcmp(pub, k.carol, 32)) << "denied depositor is not announced";
    n++;
  }
  EXPECT_EQ(n, 3);
}

TEST(MemMailboxBackend, AclCappedAtSixtyFour) {
  MemMailboxBackend be;
  uint8_t k_owner[16] = {};
  for (int i = 0; i < 70; i++) {
    uint8_t p[32] = {(uint8_t)i, 0x77};
    ASSERT_TRUE(be.addOwner(p, k_owner));
  }
  be.hello();
  uint8_t pub[32], owner4[4];
  bool is_owner;
  int n = 0;
  while (be.pollAcl(pub, is_owner, owner4)) {
    EXPECT_EQ(pub[0], n) << "owners in the order they were added";
    n++;
  }
  EXPECT_EQ(n, 64);
}

TEST(MemMailboxBackend, TimeSourceOverridesSetTime) {
  Keys k;
  MemMailboxBackend be;
  uint32_t t = 5000;
  be.setTimeSource([&t]() { return t; });
  be.setTime(1);
  registerBob(be, k);
  uint8_t hash[8] = {7}, payload[20] = {};
  be.store(2, k.owner, k.bob, hash, payload, sizeof(payload));
  BackendReply r;
  ASSERT_TRUE(be.poll(r));
  EXPECT_EQ(r.expires, 5000u + 7 * 86400);
}

TEST(MemMailboxBackend, BackendTimeIsItsClock) {
  MemMailboxBackend be;
  EXPECT_EQ(be.backendTime(), 0u) << "unknown until set";
  be.setTime(1790000000);
  EXPECT_EQ(be.backendTime(), 1790000000u);
  uint32_t t = 1790000500;
  be.setTimeSource([&t]() { return t; });
  EXPECT_EQ(be.backendTime(), 1790000500u);
}

TEST(MemMailboxBackend, AddOwnerRejectsPrefixCollisionAndBadLimits) {
  MemMailboxBackend be;
  uint8_t a[32] = {1, 2, 3, 4, 5}, b[32] = {1, 2, 3, 4, 6}, k[16] = {};
  EXPECT_TRUE(be.addOwner(a, k));
  EXPECT_FALSE(be.addOwner(b, k));
  EXPECT_FALSE(be.addOwner(a, k, 0));
  EXPECT_FALSE(be.addOwner(a, k, 31));
  EXPECT_FALSE(be.addOwner(a, k, 7, 0));
  EXPECT_FALSE(be.addOwner(a, k, 7, 30, 0));
  uint8_t unknown[4] = {9, 9, 9, 9};
  EXPECT_FALSE(be.deny(unknown, a));
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
