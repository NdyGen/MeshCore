#include "RdmCodec.h"

#include "RdmBytes.h"

#include <string.h>

namespace rdm { namespace codec {

// BaseChatMesh.h MAX_TEXT_LEN: a DM without the RDM trailer keeps the upstream limits.
static const size_t UPSTREAM_MAX_TEXT_LEN = 160;
static const uint8_t CTRL_FLAGS = TXT_TYPE_RDM_CTRL << 2;
static const size_t REG_REQ_LEN = 19;
static const size_t REG_RESP_LEN = 7;
static const size_t DEPOSIT_RESP_LEN = 13;
static const size_t REPORT_LEN = 8 + 1 + 6;
static const size_t QUERY_ITEM_LEN = 4 + 4;
static const size_t REPLY_ITEM_LEN = 1 + 6;

// from may lie past the end (parseTxtPlain: a NUL as the last byte)
static bool zeroTail(const uint8_t* d, size_t from, size_t len) {
  return from >= len || isZero(d + from, len - from);
}

static bool validCode(uint8_t c) {
  return c == (uint8_t)MbxCode::OK || c == (uint8_t)MbxCode::ALREADY_STORED ||
         (c >= (uint8_t)MbxCode::NOT_AUTH && c <= (uint8_t)MbxCode::RATE_LIMITED);
}

size_t buildTxtPlain(uint8_t* out, uint32_t ts, uint8_t txt_type, uint8_t attempt,
                     const char* text, size_t text_len, bool cap_trailer) {
  size_t max = cap_trailer ? MAX_TEXT : (attempt > 3 ? UPSTREAM_MAX_TEXT_LEN - 2 : UPSTREAM_MAX_TEXT_LEN);
  if (text_len > max) return 0;
  // a NUL inside the text would end it early at the receiver and be misread as the trailer
  if (memchr(text, 0, text_len) != NULL) return 0;

  put32(out, ts);
  out[4] = (uint8_t)((attempt & 3) | (txt_type << 2));
  memcpy(&out[5], text, text_len);
  size_t n = 5 + text_len;
  if (cap_trailer || attempt > 3) {
    out[n++] = 0;
    out[n++] = attempt;
    if (cap_trailer) out[n++] = CAP_BYTE;
  }
  return n;
}

// Lenient on purpose: whatever upstream would display must parse, so only the cap detection is strict.
bool parseTxtPlain(const uint8_t* data, size_t len, TxtParsed& out) {
  if (len < 5) return false;
  out.ts = get32(data);
  out.flags = data[4];
  out.txt_type = data[4] >> 2;
  out.text = (const char*)&data[5];
  const uint8_t* nul = (const uint8_t*)memchr(&data[5], 0, len - 5);
  out.text_len = nul ? (size_t)(nul - &data[5]) : len - 5;
  out.cap = false;
  out.has_ext_attempt = false;
  out.ext_attempt = 0;
  if (nul == NULL) return true;

  size_t i = 5 + out.text_len + 1;
  if (i < len) out.ext_attempt = data[i];
  out.cap = i + 1 < len && data[i + 1] == CAP_BYTE && zeroTail(data, i + 2, len);
  out.has_ext_attempt = out.cap || out.ext_attempt != 0;
  return true;
}

size_t buildAckR(uint8_t out[7], const uint8_t ack_r[4], uint8_t ext_attempt, uint8_t rnd, bool cap) {
  memcpy(out, ack_r, 4);
  out[4] = ext_attempt;
  out[5] = rnd;
  if (!cap) return 6;
  out[6] = CAP_BYTE;
  return 7;
}

bool ackHasCap(const uint8_t* ack, size_t len) {
  return len >= 7 && ack[6] == CAP_BYTE;
}

size_t buildCtrlPlain(uint8_t* out, uint32_t ts, const MbxInfo& info) {
  if (info.version != CTRL_VERSION) return 0;
  if (info.sub != CTRL_MBX_INFO && info.sub != CTRL_MBX_REVOKE) return 0;
  put32(out, ts);
  out[4] = CTRL_FLAGS;
  out[5] = info.sub;
  out[6] = info.version;
  if (info.sub == CTRL_MBX_REVOKE) return CTRL_REVOKE_LEN;
  memcpy(&out[7], info.mbx_pub, 32);
  memcpy(&out[39], info.token, 8);
  return CTRL_INFO_LEN;
}

size_t ctrlPlainFromBody(uint8_t* out, uint32_t ts, const uint8_t* body, size_t len) {
  if (len > MAX_TEXT) return 0;
  put32(out, ts);
  out[4] = CTRL_FLAGS;
  memcpy(&out[5], body, len);
  return 5 + len;
}

bool parseCtrlPlain(const uint8_t* data, size_t len, uint32_t& ts, MbxInfo& out) {
  if (len < CTRL_REVOKE_LEN || data[4] != CTRL_FLAGS || data[6] != CTRL_VERSION) return false;
  memset(&out, 0, sizeof(out));
  out.sub = data[5];
  out.version = data[6];
  if (out.sub == CTRL_MBX_REVOKE) {
    if (!zeroTail(data, CTRL_REVOKE_LEN, len)) return false;
  } else if (out.sub == CTRL_MBX_INFO) {
    if (len < CTRL_INFO_LEN || !zeroTail(data, CTRL_INFO_LEN, len)) return false;
    memcpy(out.mbx_pub, &data[7], 32);
    memcpy(out.token, &data[39], 8);
  } else {
    return false;
  }
  ts = get32(data);
  return true;
}

size_t buildRegReq(uint8_t* out, uint32_t ts, const uint8_t owner[4], const uint8_t token[8]) {
  put32(out, ts);
  out[4] = REG_MARKER0;
  out[5] = REG_MARKER1;
  out[6] = REG_VERSION;
  memcpy(&out[7], owner, 4);
  memcpy(&out[11], token, 8);
  return REG_REQ_LEN;
}

bool parseRegReq(const uint8_t* data, size_t len, uint32_t& ts, uint8_t owner[4], uint8_t token[8]) {
  if (len < REG_REQ_LEN || data[4] != REG_MARKER0 || data[5] != REG_MARKER1 || data[6] != REG_VERSION) return false;
  if (!zeroTail(data, REG_REQ_LEN, len)) return false;
  ts = get32(data);
  memcpy(owner, &data[7], 4);
  memcpy(token, &data[11], 8);
  return true;
}

size_t buildRegResp(uint8_t* out, MbxCode st, uint8_t ttl_days, uint8_t quota, uint32_t time_m) {
  out[0] = (uint8_t)st;
  out[1] = ttl_days;
  out[2] = quota;
  put32(&out[3], time_m);
  return REG_RESP_LEN;
}

bool parseRegResp(const uint8_t* body, size_t len, MbxCode& st, uint8_t& ttl_days, uint8_t& quota, uint32_t& time_m) {
  if (len < REG_RESP_LEN || !validCode(body[0]) || !zeroTail(body, REG_RESP_LEN, len)) return false;
  st = (MbxCode)body[0];
  ttl_days = body[1];
  quota = body[2];
  time_m = get32(&body[3]);
  return true;
}

size_t buildDeposit(uint8_t* out, const uint8_t owner[4], const uint8_t* inner, uint8_t inner_len) {
  if (inner_len > MAX_INNER_PAYLOAD) return 0;
  out[0] = REQ_DEPOSIT;
  memcpy(&out[1], owner, 4);
  out[5] = inner_len;
  memcpy(&out[6], inner, inner_len);
  return 6 + (size_t)inner_len;
}

// An inner payload above MAX_INNER_PAYLOAD still parses: the mailbox must be able to answer TOO_BIG.
bool parseDeposit(const uint8_t* body, size_t len, uint8_t owner[4], const uint8_t*& inner, uint8_t& inner_len) {
  if (len < 6 || body[0] != REQ_DEPOSIT) return false;
  size_t end = 6 + (size_t)body[5];
  if (end > len || !zeroTail(body, end, len)) return false;
  memcpy(owner, &body[1], 4);
  inner_len = body[5];
  inner = &body[6];
  return true;
}

size_t buildFetch(uint8_t* out, uint8_t flags, uint32_t store_id, const Report* r, uint8_t n) {
  if (n > MAX_BATCH) return 0;
  out[0] = REQ_FETCH;
  out[1] = flags;
  put32(&out[2], store_id);
  out[6] = n;
  uint8_t* p = &out[7];
  for (uint8_t i = 0; i < n; i++, p += REPORT_LEN) {
    memcpy(p, r[i].pkt_hash, 8);
    p[8] = (uint8_t)r[i].result;
    memcpy(&p[9], r[i].ack, 6);
  }
  return 7 + n * REPORT_LEN;
}

bool parseFetch(const uint8_t* body, size_t len, uint8_t& flags, uint32_t& store_id, Report* r, uint8_t& n) {
  if (len < 7 || body[0] != REQ_FETCH || body[6] > MAX_BATCH) return false;
  size_t end = 7 + body[6] * REPORT_LEN;
  if (end > len || !zeroTail(body, end, len)) return false;
  const uint8_t* p = &body[7];
  for (uint8_t i = 0; i < body[6]; i++, p += REPORT_LEN) {
    if (p[8] > (uint8_t)ReportResult::INBOX_FULL) return false;
  }
  flags = body[1];
  store_id = get32(&body[2]);
  n = body[6];
  p = &body[7];
  for (uint8_t i = 0; i < n; i++, p += REPORT_LEN) {
    memcpy(r[i].pkt_hash, p, 8);
    r[i].result = (ReportResult)p[8];
    memcpy(r[i].ack, &p[9], 6);
  }
  return true;
}

size_t buildStatus(uint8_t* out, const uint8_t (*hashes)[8], uint8_t n) {
  if (n == 0 || n > MAX_BATCH) return 0;
  out[0] = REQ_STATUS;
  out[1] = n;
  memcpy(&out[2], hashes, n * 8);
  return 2 + n * 8;
}

bool parseStatus(const uint8_t* body, size_t len, uint8_t (*hashes)[8], uint8_t& n) {
  if (len < 2 || body[0] != REQ_STATUS || body[1] == 0 || body[1] > MAX_BATCH) return false;
  size_t end = 2 + body[1] * 8;
  if (end > len || !zeroTail(body, end, len)) return false;
  n = body[1];
  memcpy(hashes, &body[2], n * 8);
  return true;
}

size_t buildQuery(uint8_t* out, const QueryItem* q, uint8_t n) {
  if (n == 0 || n > MAX_BATCH) return 0;
  out[0] = REQ_RECEIPT_QUERY;
  out[1] = n;
  uint8_t* p = &out[2];
  for (uint8_t i = 0; i < n; i++, p += QUERY_ITEM_LEN) {
    put32(p, q[i].ts);
    memcpy(&p[4], q[i].key, 4);
  }
  return 2 + n * QUERY_ITEM_LEN;
}

bool parseQuery(const uint8_t* body, size_t len, QueryItem* q, uint8_t& n) {
  if (len < 2 || body[0] != REQ_RECEIPT_QUERY || body[1] == 0 || body[1] > MAX_BATCH) return false;
  size_t end = 2 + body[1] * QUERY_ITEM_LEN;
  if (end > len || !zeroTail(body, end, len)) return false;
  n = body[1];
  const uint8_t* p = &body[2];
  for (uint8_t i = 0; i < n; i++, p += QUERY_ITEM_LEN) {
    q[i].ts = get32(p);
    memcpy(q[i].key, &p[4], 4);
  }
  return true;
}

size_t buildDepositResp(uint8_t* out, MbxCode st, const uint8_t pkt_hash[8], uint32_t ttl_s) {
  out[0] = (uint8_t)st;
  memcpy(&out[1], pkt_hash, 8);
  put32(&out[9], ttl_s);
  return DEPOSIT_RESP_LEN;
}

bool parseDepositResp(const uint8_t* body, size_t len, MbxCode& st, uint8_t pkt_hash[8], uint32_t& ttl_s) {
  if (len < DEPOSIT_RESP_LEN || !validCode(body[0]) || !zeroTail(body, DEPOSIT_RESP_LEN, len)) return false;
  st = (MbxCode)body[0];
  memcpy(pkt_hash, &body[1], 8);
  ttl_s = get32(&body[9]);
  return true;
}

size_t buildFetchResp(uint8_t* out, uint8_t remaining, uint8_t reports_ok, const uint8_t* inner, uint8_t inner_len) {
  if (inner_len > MAX_INNER_PAYLOAD || reports_ok > MAX_BATCH) return 0;
  out[0] = remaining;
  out[1] = reports_ok;
  out[2] = inner_len;
  if (inner_len) memcpy(&out[3], inner, inner_len);
  return 3 + (size_t)inner_len;
}

bool parseFetchResp(const uint8_t* body, size_t len, uint8_t& remaining, uint8_t& reports_ok,
                    const uint8_t*& inner, uint8_t& inner_len) {
  if (len < 3 || body[1] > MAX_BATCH || body[2] > MAX_INNER_PAYLOAD) return false;
  size_t end = 3 + (size_t)body[2];
  if (end > len || !zeroTail(body, end, len)) return false;
  remaining = body[0];
  reports_ok = body[1];
  inner_len = body[2];
  inner = &body[3];
  return true;
}

// STATUS and RECEIPT_QUERY replies share the layout n | n x (state | ack6); only the reply type and its state
// range differ.
template <class Reply>
static size_t buildReplyItems(uint8_t* out, const Reply* r, uint8_t n) {
  if (n == 0 || n > MAX_BATCH) return 0;
  out[0] = n;
  uint8_t* p = &out[1];
  for (uint8_t i = 0; i < n; i++, p += REPLY_ITEM_LEN) {
    p[0] = (uint8_t)r[i].state;
    memcpy(&p[1], r[i].ack, 6);
  }
  return 1 + n * REPLY_ITEM_LEN;
}

template <class Reply, class State>
static bool parseReplyItems(const uint8_t* body, size_t len, State max_state, Reply* r, uint8_t& n) {
  if (len < 1 || body[0] == 0 || body[0] > MAX_BATCH) return false;
  size_t end = 1 + body[0] * REPLY_ITEM_LEN;
  if (end > len || !zeroTail(body, end, len)) return false;
  for (uint8_t i = 0; i < body[0]; i++) {
    if (body[1 + i * REPLY_ITEM_LEN] > (uint8_t)max_state) return false;
  }
  n = body[0];
  for (uint8_t i = 0; i < n; i++) {
    const uint8_t* p = &body[1 + i * REPLY_ITEM_LEN];
    r[i].state = (State)p[0];
    memcpy(r[i].ack, &p[1], 6);
  }
  return true;
}

size_t buildStatusResp(uint8_t* out, const StatusReply* r, uint8_t n) { return buildReplyItems(out, r, n); }

bool parseStatusResp(const uint8_t* body, size_t len, StatusReply* r, uint8_t& n) {
  return parseReplyItems(body, len, MbxState::SYNC_EXPIRED, r, n);
}

size_t buildQueryResp(uint8_t* out, const QueryReply* r, uint8_t n) { return buildReplyItems(out, r, n); }

bool parseQueryResp(const uint8_t* body, size_t len, QueryReply* r, uint8_t& n) {
  return parseReplyItems(body, len, QueryState::EVICTED, r, n);
}

}}
