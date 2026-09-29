#pragma once

#include "RdmTypes.h"

// build* return the number of bytes written (0 = error). parse* accept trailing zero padding (decrypted data is
// padded to a 16-byte block) and reject short or inconsistent input. REQ bodies start at req_type (the adapter
// prepends the 4-byte timestamp); RESPONSE bodies start after the 4-byte tag.

namespace rdm { namespace codec {

// TXT plaintext: ts(4) | flags(1) | text | [0x00 | attempt | CAP_BYTE]
size_t buildTxtPlain(uint8_t* out, uint32_t ts, uint8_t txt_type, uint8_t attempt,
                     const char* text, size_t text_len, bool cap_trailer);
struct TxtParsed {
  uint32_t ts; uint8_t flags; uint8_t txt_type;
  const char* text; size_t text_len;
  bool has_ext_attempt; uint8_t ext_attempt;   // byte after the NUL (upstream precedent)
  bool cap;                                    // second byte after the NUL == CAP_BYTE
};
bool parseTxtPlain(const uint8_t* data, size_t len, TxtParsed& out);

// ACK bytes: ack_r(4) | ext_attempt(1) | random(1) [| CAP_BYTE]   -> 6 or 7 bytes
size_t buildAckR(uint8_t out[7], const uint8_t ack_r[4], uint8_t ext_attempt, uint8_t rnd, bool cap);
bool   ackHasCap(const uint8_t* ack, size_t len);                  // len >= 7 && ack[6] == CAP_BYTE

// CTRL (txt_type 8): ts(4) | 0x20 | sub(1) | version(1) | [mbx_pub(32) | token(8)]  -> 47 or 7 bytes
struct MbxInfo { uint8_t sub; uint8_t version; uint8_t mbx_pub[32]; uint8_t token[8]; };
size_t buildCtrlPlain(uint8_t* out, uint32_t ts, const MbxInfo& info);
bool   parseCtrlPlain(const uint8_t* data, size_t len, uint32_t& ts, MbxInfo& out);

// Registration (ANON_REQ plaintext): ts(4) | 0xFF | 'M' | version(1) | owner(4) | token(8)  -> 19 bytes
size_t buildRegReq(uint8_t* out, uint32_t ts, const uint8_t owner[4], const uint8_t token[8]);
bool   parseRegReq(const uint8_t* data, size_t len, uint32_t& ts, uint8_t owner[4], uint8_t token[8]);
// Reply (RESPONSE body after tag): status(1) | ttl_days(1) | quota(1) | time_m(4)
size_t buildRegResp(uint8_t* out, MbxCode st, uint8_t ttl_days, uint8_t quota, uint32_t time_m);
bool   parseRegResp(const uint8_t* body, size_t len, MbxCode& st, uint8_t& ttl_days, uint8_t& quota, uint32_t& time_m);

// REQ bodies
size_t buildDeposit(uint8_t* out, const uint8_t owner[4], const uint8_t* inner, uint8_t inner_len);
bool   parseDeposit(const uint8_t* body, size_t len, uint8_t owner[4], const uint8_t*& inner, uint8_t& inner_len);
size_t buildFetch(uint8_t* out, uint8_t flags, uint32_t store_id, const Report* r, uint8_t n);
bool   parseFetch(const uint8_t* body, size_t len, uint8_t& flags, uint32_t& store_id, Report* r, uint8_t& n);
size_t buildStatus(uint8_t* out, const uint8_t (*hashes)[8], uint8_t n);
bool   parseStatus(const uint8_t* body, size_t len, uint8_t (*hashes)[8], uint8_t& n);
size_t buildQuery(uint8_t* out, const QueryItem* q, uint8_t n);
bool   parseQuery(const uint8_t* body, size_t len, QueryItem* q, uint8_t& n);

// RESPONSE bodies (after tag)
size_t buildDepositResp(uint8_t* out, MbxCode st, const uint8_t pkt_hash[8], uint32_t ttl_s);   // seconds until M expires it
bool   parseDepositResp(const uint8_t* body, size_t len, MbxCode& st, uint8_t pkt_hash[8], uint32_t& ttl_s);
size_t buildFetchResp(uint8_t* out, uint8_t remaining, uint8_t reports_ok, const uint8_t* inner, uint8_t inner_len);
bool   parseFetchResp(const uint8_t* body, size_t len, uint8_t& remaining, uint8_t& reports_ok,
                      const uint8_t*& inner, uint8_t& inner_len);
size_t buildStatusResp(uint8_t* out, const StatusReply* r, uint8_t n);
bool   parseStatusResp(const uint8_t* body, size_t len, StatusReply* r, uint8_t& n);
size_t buildQueryResp(uint8_t* out, const QueryReply* r, uint8_t n);
bool   parseQueryResp(const uint8_t* body, size_t len, QueryReply* r, uint8_t& n);

}}
