#include <gtest/gtest.h>

#include <helpers/rdm/RdmCodec.h>

#include <MeshCore.h>
#include <Utils.h>

#include <functional>
#include <string>
#include <vector>

using namespace rdm;
using namespace rdm::codec;

namespace {

typedef std::vector<uint8_t> Bytes;
typedef std::function<bool(const uint8_t*, size_t)> Parser;

// Every proper prefix of a well-formed body must be rejected.
void expectTruncationRejected(const Bytes& full, const Parser& parse, size_t min_ok) {
  for (size_t n = 0; n < min_ok; n++) EXPECT_FALSE(parse(full.data(), n)) << "prefix of " << n << " bytes";
}

// Decrypted bodies arrive padded with zeros to a whole AES block; anything else after the body is garbage.
void expectPaddingRules(const Bytes& full, const Parser& parse) {
  Bytes padded = full;
  padded.resize(((full.size() + 15) / 16) * 16 + 16, 0);
  EXPECT_TRUE(parse(padded.data(), padded.size())) << "zero padding accepted";
  Bytes junk = full;
  junk.push_back(0x5A);
  EXPECT_FALSE(parse(junk.data(), junk.size())) << "non-zero trailing byte rejected";
}

Bytes fill(size_t n, uint8_t seed) {
  Bytes b(n);
  for (size_t i = 0; i < n; i++) b[i] = (uint8_t)(seed + i * 7);
  return b;
}

uint32_t rd32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

}

// ---- TXT plaintext ----

TEST(RdmCodecTxt, LayoutWithCapTrailer) {
  uint8_t out[200];
  size_t n = buildTxtPlain(out, 0x11223344, 0, 253, "hi", 2, true);
  const uint8_t expect[] = {0x44, 0x33, 0x22, 0x11, 253 & 3, 'h', 'i', 0x00, 253, CAP_BYTE};
  ASSERT_EQ(n, sizeof(expect));
  EXPECT_EQ(0, memcmp(out, expect, n));
}

TEST(RdmCodecTxt, RoundTripWithCap) {
  uint8_t out[200];
  for (uint8_t attempt : {0, 1, 2, 3, 252, 255, 248}) {
    size_t n = buildTxtPlain(out, 1790640000, 0, attempt, "hello", 5, true);
    ASSERT_GT(n, 0u);
    TxtParsed p;
    ASSERT_TRUE(parseTxtPlain(out, n, p));
    EXPECT_EQ(p.ts, 1790640000u);
    EXPECT_EQ(p.flags, attempt & 3);
    EXPECT_EQ(p.txt_type, 0);
    EXPECT_EQ(std::string(p.text, p.text_len), "hello");
    EXPECT_TRUE(p.cap);
    EXPECT_TRUE(p.has_ext_attempt);
    EXPECT_EQ(p.ext_attempt, attempt);

    Bytes padded(out, out + n);
    padded.resize(32, 0);
    ASSERT_TRUE(parseTxtPlain(padded.data(), padded.size(), p));
    EXPECT_TRUE(p.cap) << "zero padding after the trailer keeps the cap";
    EXPECT_EQ(p.ext_attempt, attempt);
  }
}

TEST(RdmCodecTxt, WithoutTrailerMatchesUpstreamLayout) {
  uint8_t out[200];
  size_t n = buildTxtPlain(out, 7, 0, 2, "abc", 3, false);
  ASSERT_EQ(n, 8u) << "app attempts 0-3 carry no extra bytes upstream";
  EXPECT_EQ(out[4], 2);

  n = buildTxtPlain(out, 7, 0, 254, "abc", 3, false);
  const uint8_t expect[] = {7, 0, 0, 0, 254 & 3, 'a', 'b', 'c', 0x00, 254};
  ASSERT_EQ(n, sizeof(expect)) << "attempt > 3: upstream extended attempt byte";
  EXPECT_EQ(0, memcmp(out, expect, n));
}

// Upstream sender: 0x00 | attempt followed by padding; the attempt byte may even equal CAP_BYTE.
TEST(RdmCodecTxt, StandardTrailerIsNotCap) {
  uint8_t out[200];
  for (uint8_t attempt : {4, 5, 0x81, 255}) {
    size_t n = buildTxtPlain(out, 9, 0, attempt, "std", 3, false);
    Bytes padded(out, out + n);
    padded.resize(16, 0);
    TxtParsed p;
    ASSERT_TRUE(parseTxtPlain(padded.data(), padded.size(), p));
    EXPECT_FALSE(p.cap) << "attempt " << (int)attempt;
    EXPECT_TRUE(p.has_ext_attempt);
    EXPECT_EQ(p.ext_attempt, attempt);
    EXPECT_EQ(std::string(p.text, p.text_len), "std");
  }
}

TEST(RdmCodecTxt, PlainUpstreamMessage) {
  Bytes plain = {1, 2, 3, 4, 0x01, 'o', 'k'};
  plain.resize(16, 0);
  TxtParsed p;
  ASSERT_TRUE(parseTxtPlain(plain.data(), plain.size(), p));
  EXPECT_EQ(std::string(p.text, p.text_len), "ok");
  EXPECT_FALSE(p.has_ext_attempt) << "zero after the NUL is padding, not an attempt";
  EXPECT_FALSE(p.cap);

  Bytes exact = {1, 2, 3, 4, 0x00, 'a', 'b'};  // no NUL at all: text runs to the end
  ASSERT_TRUE(parseTxtPlain(exact.data(), exact.size(), p));
  EXPECT_EQ(p.text_len, 2u);
  EXPECT_FALSE(p.has_ext_attempt);
}

TEST(RdmCodecTxt, TxtTypeInFlags) {
  uint8_t out[64];
  size_t n = buildTxtPlain(out, 1, 2, 1, "x", 1, true);
  EXPECT_EQ(out[4], (2 << 2) | 1);
  TxtParsed p;
  ASSERT_TRUE(parseTxtPlain(out, n, p));
  EXPECT_EQ(p.txt_type, 2);
  EXPECT_EQ(p.flags, (2 << 2) | 1);
}

TEST(RdmCodecTxt, CapNeedsZerosAfterIt) {
  Bytes plain = {1, 0, 0, 0, 0, 'x', 0x00, 1, CAP_BYTE, 0x33};
  TxtParsed p;
  ASSERT_TRUE(parseTxtPlain(plain.data(), plain.size(), p)) << "still a displayable upstream text";
  EXPECT_FALSE(p.cap);
}

TEST(RdmCodecTxt, LengthLimits) {
  uint8_t out[256];
  std::string t157(MAX_TEXT, 'a'), t158(MAX_TEXT + 1, 'a');
  size_t n = buildTxtPlain(out, 1, 0, 252, t157.data(), t157.size(), true);
  EXPECT_EQ(n, 5 + MAX_TEXT + 3);
  EXPECT_LE(n + CIPHER_MAC_SIZE + CIPHER_BLOCK_SIZE - 1, (size_t)MAX_PACKET_PAYLOAD) << "unchanged upstream check";
  EXPECT_EQ(buildTxtPlain(out, 1, 0, 252, t158.data(), t158.size(), true), 0u);

  std::string t160(160, 'b'), t161(161, 'b'), t159(159, 'b');
  EXPECT_EQ(buildTxtPlain(out, 1, 0, 0, t160.data(), t160.size(), false), 165u) << "upstream MAX_TEXT_LEN";
  EXPECT_EQ(buildTxtPlain(out, 1, 0, 0, t161.data(), t161.size(), false), 0u);
  EXPECT_EQ(buildTxtPlain(out, 1, 0, 252, t159.data(), t159.size(), false), 0u) << "upstream: MAX_TEXT_LEN - 2";
}

TEST(RdmCodecTxt, RejectsEmbeddedNulAndShortInput) {
  uint8_t out[64];
  EXPECT_EQ(buildTxtPlain(out, 1, 0, 0, "a\0b", 3, true), 0u);
  TxtParsed p;
  const uint8_t four[4] = {1, 2, 3, 4};
  EXPECT_FALSE(parseTxtPlain(four, 4, p));
}

// ---- ACK bytes ----

TEST(RdmCodecAck, BuildAndDetectCap) {
  const uint8_t ack_r[4] = {0xDE, 0xAD, 0xBE, 0xEF};
  uint8_t out[7];
  ASSERT_EQ(buildAckR(out, ack_r, 253, 0x42, true), 7u);
  const uint8_t expect[7] = {0xDE, 0xAD, 0xBE, 0xEF, 253, 0x42, CAP_BYTE};
  EXPECT_EQ(0, memcmp(out, expect, 7));
  EXPECT_TRUE(ackHasCap(out, 7));

  ASSERT_EQ(buildAckR(out, ack_r, 0, CAP_BYTE, false), 6u);
  EXPECT_FALSE(ackHasCap(out, 6)) << "random byte equal to CAP_BYTE is not a cap";
}

TEST(RdmCodecAck, PathExtraWithZeroPaddingHasNoCap) {
  // Standard node: PATH extra = ACK(4) | attempt | random, decrypted with zero padding.
  Bytes extra = {1, 2, 3, 4, 0, CAP_BYTE};
  extra.resize(16, 0);
  EXPECT_FALSE(ackHasCap(extra.data(), extra.size()));
  Bytes four = {1, 2, 3, 4};
  four.resize(16, 0);
  EXPECT_FALSE(ackHasCap(four.data(), four.size()));

  // Fork node: same extra plus the cap byte, then padding.
  Bytes fork = {1, 2, 3, 4, 0, 9, CAP_BYTE};
  fork.resize(16, 0);
  EXPECT_TRUE(ackHasCap(fork.data(), fork.size()));
  EXPECT_FALSE(ackHasCap(fork.data(), 6));
}

// ---- CTRL ----

TEST(RdmCodecCtrl, MbxInfoRoundTrip) {
  MbxInfo in{};
  in.sub = CTRL_MBX_INFO;
  in.version = CTRL_VERSION;
  Bytes pub = fill(32, 3), tok = fill(8, 99);
  memcpy(in.mbx_pub, pub.data(), 32);
  memcpy(in.token, tok.data(), 8);
  uint8_t out[64];
  size_t n = buildCtrlPlain(out, 0xA0B0C0D0, in);
  ASSERT_EQ(n, 47u);
  EXPECT_EQ(rd32(out), 0xA0B0C0D0u);
  EXPECT_EQ(out[4], 0x20);
  EXPECT_EQ(out[5], CTRL_MBX_INFO);
  EXPECT_EQ(out[6], CTRL_VERSION);

  Bytes full(out, out + n);
  uint32_t ts;
  MbxInfo back;
  ASSERT_TRUE(parseCtrlPlain(full.data(), full.size(), ts, back));
  EXPECT_EQ(ts, 0xA0B0C0D0u);
  EXPECT_EQ(back.sub, CTRL_MBX_INFO);
  EXPECT_EQ(back.version, CTRL_VERSION);
  EXPECT_EQ(0, memcmp(back.mbx_pub, pub.data(), 32));
  EXPECT_EQ(0, memcmp(back.token, tok.data(), 8));

  auto parse = [](const uint8_t* d, size_t l) { uint32_t t; MbxInfo m; return parseCtrlPlain(d, l, t, m); };
  expectTruncationRejected(full, parse, n);
  expectPaddingRules(full, parse);
}

TEST(RdmCodecCtrl, RevokeRoundTrip) {
  MbxInfo in{};
  in.sub = CTRL_MBX_REVOKE;
  in.version = CTRL_VERSION;
  memset(in.mbx_pub, 0xEE, 32);
  uint8_t out[64];
  size_t n = buildCtrlPlain(out, 5, in);
  ASSERT_EQ(n, 7u);
  Bytes full(out, out + n);
  uint32_t ts;
  MbxInfo back;
  ASSERT_TRUE(parseCtrlPlain(full.data(), full.size(), ts, back));
  EXPECT_EQ(back.sub, CTRL_MBX_REVOKE);
  EXPECT_EQ(back.mbx_pub[0], 0) << "REVOKE carries no mailbox";

  auto parse = [](const uint8_t* d, size_t l) { uint32_t t; MbxInfo m; return parseCtrlPlain(d, l, t, m); };
  expectTruncationRejected(full, parse, n);
  expectPaddingRules(full, parse);
}

TEST(RdmCodecCtrl, RejectsInconsistent) {
  MbxInfo in{};
  in.sub = 0x03;
  in.version = CTRL_VERSION;
  uint8_t out[64];
  EXPECT_EQ(buildCtrlPlain(out, 1, in), 0u) << "unknown sub";

  in.sub = CTRL_MBX_INFO;
  size_t n = buildCtrlPlain(out, 1, in);
  ASSERT_EQ(n, 47u);
  uint32_t ts;
  MbxInfo back;
  Bytes bad(out, out + n);
  bad[4] = 0x00;
  EXPECT_FALSE(parseCtrlPlain(bad.data(), bad.size(), ts, back)) << "flags must be txt_type 8";
  bad = Bytes(out, out + n);
  bad[5] = 0x07;
  EXPECT_FALSE(parseCtrlPlain(bad.data(), bad.size(), ts, back)) << "unknown sub";
  bad = Bytes(out, out + n);
  bad[6] = CTRL_VERSION + 1;
  EXPECT_FALSE(parseCtrlPlain(bad.data(), bad.size(), ts, back)) << "unknown version";
}

// ---- Registration ----

TEST(RdmCodecReg, RequestRoundTrip) {
  const uint8_t owner[4] = {1, 2, 3, 4};
  Bytes tok = fill(8, 50);
  uint8_t out[32];
  size_t n = buildRegReq(out, 0x01020304, owner, tok.data());
  ASSERT_EQ(n, 19u);
  EXPECT_EQ(out[4], REG_MARKER0);
  EXPECT_EQ(out[5], REG_MARKER1);
  EXPECT_EQ(out[6], REG_VERSION);

  Bytes full(out, out + n);
  uint32_t ts;
  uint8_t o[4], t[8];
  ASSERT_TRUE(parseRegReq(full.data(), full.size(), ts, o, t));
  EXPECT_EQ(ts, 0x01020304u);
  EXPECT_EQ(0, memcmp(o, owner, 4));
  EXPECT_EQ(0, memcmp(t, tok.data(), 8));

  auto parse = [](const uint8_t* d, size_t l) { uint32_t a; uint8_t b[4], c[8]; return parseRegReq(d, l, a, b, c); };
  expectTruncationRejected(full, parse, n);
  expectPaddingRules(full, parse);

  full[5] = 'X';
  EXPECT_FALSE(parse(full.data(), full.size())) << "marker";
  full[5] = REG_MARKER1;
  full[6] = REG_VERSION + 1;
  EXPECT_FALSE(parse(full.data(), full.size())) << "version";
}

TEST(RdmCodecReg, ResponseRoundTrip) {
  uint8_t out[16];
  size_t n = buildRegResp(out, MbxCode::NOT_AUTH, 7, 20, 1790640000);
  ASSERT_EQ(n, 7u);
  Bytes full(out, out + n);
  MbxCode st;
  uint8_t ttl, quota;
  uint32_t tm;
  ASSERT_TRUE(parseRegResp(full.data(), full.size(), st, ttl, quota, tm));
  EXPECT_EQ(st, MbxCode::NOT_AUTH);
  EXPECT_EQ(ttl, 7);
  EXPECT_EQ(quota, 20);
  EXPECT_EQ(tm, 1790640000u);

  auto parse = [](const uint8_t* d, size_t l) { MbxCode a; uint8_t b, c; uint32_t e; return parseRegResp(d, l, a, b, c, e); };
  expectTruncationRejected(full, parse, n);
  expectPaddingRules(full, parse);
  full[0] = 0x7F;
  EXPECT_FALSE(parse(full.data(), full.size())) << "unknown status code";
}

// ---- DEPOSIT ----

TEST(RdmCodecDeposit, RoundTrip) {
  const uint8_t owner[4] = {9, 8, 7, 6};
  Bytes inner = fill(40, 1);
  uint8_t out[200];
  size_t n = buildDeposit(out, owner, inner.data(), (uint8_t)inner.size());
  ASSERT_EQ(n, 6 + inner.size());
  EXPECT_EQ(out[0], REQ_DEPOSIT);

  Bytes full(out, out + n);
  uint8_t o[4];
  const uint8_t* in_p;
  uint8_t in_len;
  ASSERT_TRUE(parseDeposit(full.data(), full.size(), o, in_p, in_len));
  EXPECT_EQ(0, memcmp(o, owner, 4));
  ASSERT_EQ(in_len, inner.size());
  EXPECT_EQ(0, memcmp(in_p, inner.data(), in_len));
  EXPECT_EQ(in_p, full.data() + 6) << "points into the body, no copy";

  auto parse = [](const uint8_t* d, size_t l) { uint8_t a[4]; const uint8_t* b; uint8_t c; return parseDeposit(d, l, a, b, c); };
  expectTruncationRejected(full, parse, n);
  expectPaddingRules(full, parse);
  full[0] = REQ_FETCH;
  EXPECT_FALSE(parse(full.data(), full.size())) << "wrong req_type";
}

// The mailbox answers TOO_BIG itself, so an oversized but well-framed DEPOSIT must still parse.
TEST(RdmCodecDeposit, OversizedInnerParsesButIsNotBuilt) {
  const uint8_t owner[4] = {1, 1, 1, 1};
  Bytes inner = fill(MAX_INNER_PAYLOAD + 1, 2);
  uint8_t out[256];
  EXPECT_EQ(buildDeposit(out, owner, inner.data(), (uint8_t)inner.size()), 0u);

  Bytes body = {REQ_DEPOSIT, 1, 1, 1, 1, (uint8_t)inner.size()};
  body.insert(body.end(), inner.begin(), inner.end());
  uint8_t o[4];
  const uint8_t* p;
  uint8_t len;
  ASSERT_TRUE(parseDeposit(body.data(), body.size(), o, p, len));
  EXPECT_EQ(len, MAX_INNER_PAYLOAD + 1);
}

// A real mailbox copy: fork DM with trailer, encrypted like Mesh::createDatagram (dest|src|MAC|ciphertext).
static size_t mailboxDepositPlainLen(size_t text_len) {
  uint8_t secret[PUB_KEY_SIZE];
  memset(secret, 0x3C, sizeof(secret));
  std::string text(text_len, 'm');
  uint8_t plain[256];
  size_t plain_len = buildTxtPlain(plain, 1790640000, 0, 252, text.data(), text.size(), true);
  if (plain_len == 0) return 0;
  uint8_t inner[256];
  inner[0] = 0xA1;
  inner[1] = 0xB2;
  size_t inner_len = 2 * PATH_HASH_SIZE + mesh::Utils::encryptThenMAC(secret, &inner[2], plain, (int)plain_len);
  if (inner_len > 255) return 0;
  const uint8_t owner[4] = {1, 2, 3, 4};
  uint8_t body[300];
  size_t n = buildDeposit(body, owner, inner, (uint8_t)inner_len);
  return n == 0 ? 0 : 4 + n;   // plus the REQ timestamp
}

TEST(RdmCodecDeposit, MailboxTextLimit152) {
  size_t plain = mailboxDepositPlainLen(MAX_TEXT_MAILBOX);
  EXPECT_EQ(plain, 174u);
  // exact datagram check (06 par. 3.13): dest | src | MAC | pad16(plaintext) <= MAX_PACKET_PAYLOAD
  EXPECT_LE(PATH_HASH_SIZE * 2 + CIPHER_MAC_SIZE + ((plain + CIPHER_BLOCK_SIZE - 1) / CIPHER_BLOCK_SIZE) * CIPHER_BLOCK_SIZE,
            (size_t)MAX_PACKET_PAYLOAD);
  EXPECT_EQ(mailboxDepositPlainLen(MAX_TEXT_MAILBOX + 1), 0u);
}

// ---- FETCH ----

TEST(RdmCodecFetch, RoundTripWithReports) {
  Report r[MAX_BATCH];
  for (uint8_t i = 0; i < MAX_BATCH; i++) {
    for (int j = 0; j < 8; j++) r[i].pkt_hash[j] = (uint8_t)(i * 16 + j);
    r[i].result = (ReportResult)(i % 5);
    for (int j = 0; j < 6; j++) r[i].ack[j] = (uint8_t)(0x80 + i + j);
  }
  uint8_t out[200];
  size_t n = buildFetch(out, FETCH_FLAG_NO_PAYLOAD, 0xCAFEBABE, r, MAX_BATCH);
  ASSERT_EQ(n, 127u) << "06 par. 3.4 max";
  EXPECT_EQ(out[0], REQ_FETCH);

  Bytes full(out, out + n);
  uint8_t flags, cnt;
  uint32_t sid;
  Report back[MAX_BATCH];
  ASSERT_TRUE(parseFetch(full.data(), full.size(), flags, sid, back, cnt));
  EXPECT_EQ(flags, FETCH_FLAG_NO_PAYLOAD);
  EXPECT_EQ(sid, 0xCAFEBABEu);
  ASSERT_EQ(cnt, MAX_BATCH);
  for (int i = 0; i < MAX_BATCH; i++) {
    EXPECT_EQ(0, memcmp(back[i].pkt_hash, r[i].pkt_hash, 8));
    EXPECT_EQ(back[i].result, r[i].result);
    EXPECT_EQ(0, memcmp(back[i].ack, r[i].ack, 6));
  }

  auto parse = [](const uint8_t* d, size_t l) { uint8_t a, c; uint32_t b; Report rr[MAX_BATCH]; return parseFetch(d, l, a, b, rr, c); };
  expectTruncationRejected(full, parse, n);
  expectPaddingRules(full, parse);
  full[7 + 8] = 5;
  EXPECT_FALSE(parse(full.data(), full.size())) << "unknown report result";
}

TEST(RdmCodecFetch, EmptyAndTooMany) {
  uint8_t out[200];
  size_t n = buildFetch(out, 0, 1, nullptr, 0);
  ASSERT_EQ(n, 7u);
  uint8_t flags, cnt;
  uint32_t sid;
  Report back[MAX_BATCH];
  ASSERT_TRUE(parseFetch(out, n, flags, sid, back, cnt));
  EXPECT_EQ(cnt, 0);

  Report r[MAX_BATCH + 1] = {};
  EXPECT_EQ(buildFetch(out, 0, 1, r, MAX_BATCH + 1), 0u);
  Bytes body = {REQ_FETCH, 0, 1, 0, 0, 0, MAX_BATCH + 1};
  body.resize(7 + 15 * (MAX_BATCH + 1), 0);
  EXPECT_FALSE(parseFetch(body.data(), body.size(), flags, sid, back, cnt)) << "n > MAX_BATCH";
}

// ---- STATUS ----

TEST(RdmCodecStatus, RoundTrip) {
  uint8_t hashes[MAX_BATCH][8];
  for (int i = 0; i < MAX_BATCH; i++)
    for (int j = 0; j < 8; j++) hashes[i][j] = (uint8_t)(i ^ (j << 4));
  uint8_t out[100];
  size_t n = buildStatus(out, hashes, MAX_BATCH);
  ASSERT_EQ(n, 66u);
  Bytes full(out, out + n);
  uint8_t back[MAX_BATCH][8], cnt;
  ASSERT_TRUE(parseStatus(full.data(), full.size(), back, cnt));
  ASSERT_EQ(cnt, MAX_BATCH);
  EXPECT_EQ(0, memcmp(back, hashes, sizeof(hashes)));

  auto parse = [](const uint8_t* d, size_t l) { uint8_t h[MAX_BATCH][8], c; return parseStatus(d, l, h, c); };
  expectTruncationRejected(full, parse, n);
  expectPaddingRules(full, parse);
  EXPECT_EQ(buildStatus(out, hashes, 0), 0u) << "a STATUS asks for at least one message";
  EXPECT_EQ(buildStatus(out, hashes, MAX_BATCH + 1), 0u);
}

// ---- RECEIPT_QUERY ----

TEST(RdmCodecQuery, RoundTrip) {
  QueryItem q[3];
  for (int i = 0; i < 3; i++) {
    q[i].ts = 1790640000 + i;
    for (int j = 0; j < 4; j++) q[i].key[j] = (uint8_t)(i * 4 + j + 1);
  }
  uint8_t out[100];
  size_t n = buildQuery(out, q, 3);
  ASSERT_EQ(n, 2u + 3 * 8);
  EXPECT_EQ(out[0], REQ_RECEIPT_QUERY);
  Bytes full(out, out + n);
  QueryItem back[MAX_BATCH];
  uint8_t cnt;
  ASSERT_TRUE(parseQuery(full.data(), full.size(), back, cnt));
  ASSERT_EQ(cnt, 3);
  for (int i = 0; i < 3; i++) {
    EXPECT_EQ(back[i].ts, q[i].ts);
    EXPECT_EQ(0, memcmp(back[i].key, q[i].key, 4));
  }

  auto parse = [](const uint8_t* d, size_t l) { QueryItem qq[MAX_BATCH]; uint8_t c; return parseQuery(d, l, qq, c); };
  expectTruncationRejected(full, parse, n);
  expectPaddingRules(full, parse);

  QueryItem many[MAX_BATCH] = {};
  EXPECT_EQ(buildQuery(out, many, MAX_BATCH), 66u) << "06 par. 3.4 max";
  EXPECT_EQ(buildQuery(out, many, 0), 0u);
}

// ---- Responses ----

TEST(RdmCodecResp, DepositReply) {
  const uint8_t hash[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  uint8_t out[32];
  size_t n = buildDepositResp(out, MbxCode::ALREADY_STORED, hash, 7 * 86400 - 5);   // ttl_s: relative to M's clock
  ASSERT_EQ(n, 13u);
  EXPECT_EQ(out[9], (uint8_t)((7 * 86400 - 5) & 0xFF)) << "little-endian ttl_s after status and hash";
  Bytes full(out, out + n);
  MbxCode st;
  uint8_t h[8];
  uint32_t ttl_s;
  ASSERT_TRUE(parseDepositResp(full.data(), full.size(), st, h, ttl_s));
  EXPECT_EQ(st, MbxCode::ALREADY_STORED);
  EXPECT_EQ(0, memcmp(h, hash, 8));
  EXPECT_EQ(ttl_s, 7u * 86400 - 5);

  auto parse = [](const uint8_t* d, size_t l) { MbxCode a; uint8_t b[8]; uint32_t c; return parseDepositResp(d, l, a, b, c); };
  expectTruncationRejected(full, parse, n);
  expectPaddingRules(full, parse);
  for (uint8_t code : {0x00, 0x01, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15}) {
    full[0] = code;
    EXPECT_TRUE(parse(full.data(), full.size())) << (int)code;
  }
  for (uint8_t code : {0x02, 0x0F, 0x16, 0xFF}) {
    full[0] = code;
    EXPECT_FALSE(parse(full.data(), full.size())) << (int)code;
  }
}

TEST(RdmCodecResp, FetchReplyWithCopy) {
  Bytes inner = fill(MAX_INNER_PAYLOAD, 11);
  uint8_t out[200];
  size_t n = buildFetchResp(out, 3, 2, inner.data(), (uint8_t)inner.size());
  ASSERT_EQ(n, 167u) << "06 par. 3.4 max";
  EXPECT_LE(4 + n, 176u) << "tag + body fits the exact datagram check";
  Bytes full(out, out + n);
  uint8_t rem, ok, len;
  const uint8_t* p;
  ASSERT_TRUE(parseFetchResp(full.data(), full.size(), rem, ok, p, len));
  EXPECT_EQ(rem, 3);
  EXPECT_EQ(ok, 2);
  ASSERT_EQ(len, MAX_INNER_PAYLOAD);
  EXPECT_EQ(0, memcmp(p, inner.data(), len));

  auto parse = [](const uint8_t* d, size_t l) { uint8_t a, b, c; const uint8_t* e; return parseFetchResp(d, l, a, b, e, c); };
  expectTruncationRejected(full, parse, n);
  EXPECT_EQ(buildFetchResp(out, 0, 0, inner.data(), MAX_INNER_PAYLOAD + 1), 0u);
}

TEST(RdmCodecResp, FetchReplyWithoutCopy) {
  uint8_t out[16];
  size_t n = buildFetchResp(out, 5, 0, nullptr, 0);
  ASSERT_EQ(n, 3u);
  Bytes full(out, out + n);
  uint8_t rem, ok, len;
  const uint8_t* p;
  ASSERT_TRUE(parseFetchResp(full.data(), full.size(), rem, ok, p, len));
  EXPECT_EQ(rem, 5);
  EXPECT_EQ(len, 0);

  auto parse = [](const uint8_t* d, size_t l) { uint8_t a, b, c; const uint8_t* e; return parseFetchResp(d, l, a, b, e, c); };
  expectTruncationRejected(full, parse, n);
  expectPaddingRules(full, parse);
  full[1] = MAX_BATCH + 1;
  EXPECT_FALSE(parse(full.data(), full.size())) << "more confirmed reports than a FETCH carries";
}

TEST(RdmCodecResp, StatusReply) {
  StatusReply r[2];
  r[0].state = MbxState::STORED;
  memset(r[0].ack, 0, 6);
  r[1].state = MbxState::DELIVERED;
  for (int j = 0; j < 6; j++) r[1].ack[j] = (uint8_t)(0xA0 + j);
  uint8_t out[64];
  size_t n = buildStatusResp(out, r, 2);
  ASSERT_EQ(n, 15u);
  Bytes full(out, out + n);
  StatusReply back[MAX_BATCH];
  uint8_t cnt;
  ASSERT_TRUE(parseStatusResp(full.data(), full.size(), back, cnt));
  ASSERT_EQ(cnt, 2);
  EXPECT_EQ(back[1].state, MbxState::DELIVERED);
  EXPECT_EQ(0, memcmp(back[1].ack, r[1].ack, 6));

  auto parse = [](const uint8_t* d, size_t l) { StatusReply s[MAX_BATCH]; uint8_t c; return parseStatusResp(d, l, s, c); };
  expectTruncationRejected(full, parse, n);
  expectPaddingRules(full, parse);
  full[1] = (uint8_t)MbxState::SYNC_EXPIRED + 1;
  EXPECT_FALSE(parse(full.data(), full.size())) << "unknown state";

  StatusReply eight[MAX_BATCH] = {};
  EXPECT_EQ(buildStatusResp(out, eight, MAX_BATCH), 57u) << "06 par. 3.4 max";
}

TEST(RdmCodecResp, QueryReply) {
  QueryReply r[MAX_BATCH];
  for (int i = 0; i < MAX_BATCH; i++) {
    r[i].state = (QueryState)(i % 4);
    for (int j = 0; j < 6; j++) r[i].ack[j] = (uint8_t)(i * 6 + j);
  }
  uint8_t out[64];
  size_t n = buildQueryResp(out, r, MAX_BATCH);
  ASSERT_EQ(n, 57u);
  Bytes full(out, out + n);
  QueryReply back[MAX_BATCH];
  uint8_t cnt;
  ASSERT_TRUE(parseQueryResp(full.data(), full.size(), back, cnt));
  ASSERT_EQ(cnt, MAX_BATCH);
  for (int i = 0; i < MAX_BATCH; i++) {
    EXPECT_EQ(back[i].state, r[i].state);
    EXPECT_EQ(0, memcmp(back[i].ack, r[i].ack, 6));
  }

  auto parse = [](const uint8_t* d, size_t l) { QueryReply q[MAX_BATCH]; uint8_t c; return parseQueryResp(d, l, q, c); };
  expectTruncationRejected(full, parse, n);
  expectPaddingRules(full, parse);
  full[1] = (uint8_t)QueryState::EVICTED + 1;
  EXPECT_FALSE(parse(full.data(), full.size())) << "unknown state";
}

TEST(RdmCodecResp, RepliesNeedAtLeastOneItem) {
  uint8_t out[16];
  EXPECT_EQ(buildQueryResp(out, nullptr, 0), 0u);
  EXPECT_EQ(buildStatusResp(out, nullptr, 0), 0u);
  const uint8_t zero_n[16] = {0};
  QueryReply q[MAX_BATCH];
  StatusReply s[MAX_BATCH];
  uint8_t cnt;
  EXPECT_FALSE(parseQueryResp(zero_n, sizeof(zero_n), q, cnt));
  EXPECT_FALSE(parseStatusResp(zero_n, sizeof(zero_n), s, cnt));
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
