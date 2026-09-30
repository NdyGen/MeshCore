#include "MailboxCore.h"

#include <helpers/rdm/RdmBytes.h>
#include <helpers/rdm/RdmCodec.h>
#include <helpers/rdm/RdmConfig.h>
#include <helpers/rdm/RdmCrypto.h>

#include <string.h>

namespace rdm {

namespace {

const uint32_t BACKEND_TIMEOUT_MS = 3000;
const uint32_t REQ_GAP_MS = RDM_MBX_SERVER_REQ_GAP_S * 1000UL;
const uint32_t HOUR_MS = 3600UL * 1000;
const uint8_t  REQ_PER_HOUR = 60;
const uint32_t ANON_WINDOW_MS = 60000;
const uint8_t  ANON_PER_WINDOW = 4;           // global, like anon_limiter in PR #3078

bool due(uint32_t now, uint32_t at) { return (int32_t)(now - at) >= 0; }

}

MailboxCore::MailboxCore(MailboxBackend& be, MailboxCoreHost& host) : _be(be), _host(host) { begin(); }

void MailboxCore::begin() {
  memset(_clients, 0, sizeof(_clients));
  memset(_pending, 0, sizeof(_pending));
  memset(_out, 0, sizeof(_out));
  memset(_owner_ttl, 0, sizeof(_owner_ttl));
  _out_head = _out_count = 0;
  _next_id = 1;
  _lru_seq = 0;
  _anon_window_set = false;
  _anon_count = 0;
}

MailboxCore::Client* MailboxCore::client(const uint8_t pub[32]) {
  Client* victim = &_clients[0];
  for (auto& c : _clients) {
    if (c.used && memcmp(c.pub, pub, 32) == 0) {
      c.lru = ++_lru_seq;
      return &c;
    }
    if (!c.used) {
      if (victim->used) victim = &c;
    } else if (victim->used && c.lru < victim->lru) {
      victim = &c;
    }
  }
  // Evicting forgets that client's replay and rate state; the host's peer cache has the same size, so an
  // evicted client first has to register again anyway.
  memset(victim, 0, sizeof(*victim));
  victim->used = true;
  memcpy(victim->pub, pub, 32);
  victim->lru = ++_lru_seq;
  return victim;
}

bool MailboxCore::rateLimited(const Client& c, uint32_t now) const {
  if (c.has_req && !due(now, c.last_req_ms + REQ_GAP_MS)) return true;
  return c.has_req && !due(now, c.hour_start_ms + HOUR_MS) && c.hour_count >= REQ_PER_HOUR;
}

// Rate limit per client; false: over the limit (the caller answers RATE_LIMITED where the reply has a status).
// The replay check (ts < last ts) comes before this: a replay is dropped without any answer.
bool MailboxCore::admit(Client& c, uint32_t ts, uint32_t now) {
  if (rateLimited(c, now)) return false;
  c.has_ts = true;
  c.last_ts = ts;
  if (!c.has_req || due(now, c.hour_start_ms + HOUR_MS)) {
    c.hour_start_ms = now;
    c.hour_count = 0;
  }
  c.has_req = true;
  c.last_req_ms = now;
  c.hour_count++;
  return true;
}

MailboxCore::Pending* MailboxCore::newPending(BackendReply::Kind kind, const ReplyTo& to, uint32_t now) {
  for (auto& p : _pending) {
    if (p.used) continue;
    memset(&p, 0, sizeof(p));
    p.used = true;
    p.kind = kind;
    p.id = _next_id++;
    if (_next_id == 0) _next_id = 1;
    p.to = to;
    p.deadline_ms = now + BACKEND_TIMEOUT_MS;
    return &p;
  }
  return nullptr;
}

void MailboxCore::respond(const ReplyTo& to, const uint8_t* body, size_t len) {
  if (len == 0 || len > RESP_MAX || _out_count == OUTQ) return;
  Outgoing& o = _out[(_out_head + _out_count) % OUTQ];
  o.to = to;
  o.len = (uint8_t)len;
  memcpy(o.body, body, len);
  _out_count++;
}

void MailboxCore::answerStore(const ReplyTo& to, const uint8_t pkt_hash[8], MbxCode code, uint32_t ttl_s) {
  uint8_t body[16];
  respond(to, body, codec::buildDepositResp(body, code, pkt_hash, ttl_s));
}

void MailboxCore::answerReg(const ReplyTo& to, MbxCode code, uint8_t ttl_days, uint8_t quota) {
  uint8_t body[8];
  respond(to, body, codec::buildRegResp(body, code, ttl_days, quota, _be.backendTime()));
}

void MailboxCore::rememberOwnerTtl(const uint8_t owner[4], uint8_t ttl_days) {
  OwnerTtl* slot = &_owner_ttl[0];
  for (auto& o : _owner_ttl) {
    if (o.used && memcmp(o.owner, owner, 4) == 0) { slot = &o; break; }
    if (!o.used && slot->used) slot = &o;
  }
  slot->used = true;
  memcpy(slot->owner, owner, 4);
  slot->ttl_days = ttl_days;
}

uint32_t MailboxCore::fallbackTtl(const uint8_t owner[4]) const {
  for (auto& o : _owner_ttl)
    if (o.used && memcmp(o.owner, owner, 4) == 0) return o.ttl_days * 86400UL;
  return RDM_T_RADIO_S;
}

void MailboxCore::onAnonRequest(const uint8_t client_pub[32], const uint8_t* plain, size_t len, bool via_flood) {
  uint32_t ts;
  uint8_t owner[4], token[8];
  if (!codec::parseRegReq(plain, len, ts, owner, token)) return;

  uint32_t now = _host.millis();
  // Global limit first: anyone can make up a key pair, so these are not answered at all.
  if (!_anon_window_set || due(now, _anon_window_ms + ANON_WINDOW_MS)) {
    _anon_window_set = true;
    _anon_window_ms = now;
    _anon_count = 0;
  }
  if (_anon_count >= ANON_PER_WINDOW) return;
  _anon_count++;

  Client& c = *client(client_pub);
  if (c.has_ts && ts < c.last_ts) return;
  ReplyTo to;
  memcpy(to.client, client_pub, 32);
  to.tag = ts;
  to.path_return = via_flood;   // a flood registration is answered with a PATH return, so the client learns the route
  if (!admit(c, ts, now)) {
    answerReg(to, MbxCode::RATE_LIMITED, 0, 0);
    return;
  }
  if (!_be.ready()) {
    answerReg(to, MbxCode::NO_STORAGE, 0, 0);
    return;
  }
  Pending* p = newPending(BackendReply::Kind::REG, to, now);
  if (!p) {
    answerReg(to, MbxCode::NO_STORAGE, 0, 0);
    return;
  }
  memcpy(p->owner, owner, 4);
  if (!_be.reg(p->id, client_pub, owner, token)) {
    answerReg(to, MbxCode::NO_STORAGE, 0, 0);
    p->used = false;
  }
}

void MailboxCore::onDeposit(const ReplyTo& to, const uint8_t owner[4], const uint8_t* inner, uint8_t inner_len,
                            bool limited, uint32_t now) {
  uint8_t pkt_hash[8];
  crypto::txtPacketHash(pkt_hash, inner, inner_len);
  if (limited) return answerStore(to, pkt_hash, MbxCode::RATE_LIMITED, 0);
  if (inner_len > MAX_INNER_PAYLOAD) return answerStore(to, pkt_hash, MbxCode::TOO_BIG, 0);
  if (!_be.ready()) return answerStore(to, pkt_hash, MbxCode::NO_STORAGE, 0);
  Pending* p = newPending(BackendReply::Kind::STORE, to, now);
  if (!p) return answerStore(to, pkt_hash, MbxCode::NO_STORAGE, 0);
  memcpy(p->owner, owner, 4);
  memcpy(p->pkt_hash, pkt_hash, 8);
  if (!_be.store(p->id, owner, to.client, pkt_hash, inner, inner_len)) {
    answerStore(to, pkt_hash, MbxCode::NO_STORAGE, 0);
    p->used = false;
  }
}

void MailboxCore::onRequest(const uint8_t client_pub[32], const uint8_t* data, size_t len, bool via_flood) {
  if (len < 5) return;
  uint32_t ts = get32(data);
  const uint8_t* body = data + 4;
  size_t blen = len - 4;
  uint32_t now = _host.millis();

  uint8_t owner[4];
  const uint8_t* inner = nullptr;
  uint8_t inner_len = 0;
  uint8_t flags = 0;
  uint32_t store_id = 0;
  Report reports[MAX_BATCH];
  uint8_t hashes[MAX_BATCH][8];
  uint8_t n = 0;
  BackendReply::Kind kind;
  switch (body[0]) {
    case REQ_DEPOSIT:
      if (!codec::parseDeposit(body, blen, owner, inner, inner_len)) return;
      kind = BackendReply::Kind::STORE;
      break;
    case REQ_FETCH:
      if (!codec::parseFetch(body, blen, flags, store_id, reports, n)) return;
      kind = BackendReply::Kind::FETCH;
      break;
    case REQ_STATUS:
      if (!codec::parseStatus(body, blen, hashes, n)) return;
      kind = BackendReply::Kind::STAT;
      break;
    default:
      return;
  }

  Client& c = *client(client_pub);
  if (c.has_ts && ts < c.last_ts) return;
  bool limited = !admit(c, ts, now);
  ReplyTo to;
  memcpy(to.client, client_pub, 32);
  to.tag = ts;
  to.path_return = via_flood;

  if (kind == BackendReply::Kind::STORE) return onDeposit(to, owner, inner, inner_len, limited, now);

  // FETCH and STATUS replies carry no status: when M cannot answer properly it stays silent.
  if (limited || !_be.ready()) return;
  Pending* p = newPending(kind, to, now);
  if (!p) return;
  bool sent;
  if (kind == BackendReply::Kind::FETCH) {
    // A PATH return cannot carry a 171-byte reply, so a flood FETCH only learns the count (03 par. 3).
    if (via_flood) flags |= FETCH_FLAG_NO_PAYLOAD;
    sent = _be.fetch(p->id, client_pub, flags, store_id, reports, n);
  } else {
    sent = _be.stat(p->id, client_pub, hashes, n);
  }
  if (!sent) p->used = false;
}

void MailboxCore::onBackendReply(const BackendReply& r) {
  Pending* p = nullptr;
  for (auto& q : _pending)
    if (q.used && q.id == r.id && q.kind == r.kind) p = &q;
  if (!p) return;   // timed out already, or not ours
  p->used = false;

  switch (r.kind) {
    case BackendReply::Kind::STORE: {
      uint32_t ttl_s = 0;
      if (r.code == MbxCode::OK || r.code == MbxCode::ALREADY_STORED) {
        uint32_t t = _be.backendTime();
        if (t == 0) ttl_s = fallbackTtl(p->owner);
        else ttl_s = r.expires > t ? r.expires - t : 0;
      }
      answerStore(p->to, p->pkt_hash, r.code, ttl_s);
      break;
    }
    case BackendReply::Kind::REG:
      if (r.code == MbxCode::OK) {
        _host.addPeer(p->to.client);
        rememberOwnerTtl(p->owner, r.ttl_days);
      }
      answerReg(p->to, r.code, r.ttl_days, r.quota);
      break;
    case BackendReply::Kind::FETCH: {
      if (r.code != MbxCode::OK) break;
      uint8_t body[RESP_MAX];
      uint8_t inner_len = p->to.path_return ? 0 : r.inner_len;
      respond(p->to, body, codec::buildFetchResp(body, r.remaining, r.reports_ok, r.inner, inner_len));
      break;
    }
    case BackendReply::Kind::STAT: {
      if (r.code != MbxCode::OK) break;
      uint8_t body[1 + MAX_BATCH * 7];
      respond(p->to, body, codec::buildStatusResp(body, r.items, r.n));
      break;
    }
  }
}

void MailboxCore::loop() {
  uint8_t pub[32], owner[4];
  bool is_owner;
  while (_be.pollAcl(pub, is_owner, owner)) _host.addPeer(pub);

  BackendReply r;
  while (_be.poll(r)) onBackendReply(r);

  uint32_t now = _host.millis();
  for (auto& p : _pending) {
    if (!p.used || !due(now, p.deadline_ms)) continue;
    p.used = false;
    if (p.kind == BackendReply::Kind::STORE) answerStore(p.to, p.pkt_hash, MbxCode::NO_STORAGE, 0);
    else if (p.kind == BackendReply::Kind::REG) answerReg(p.to, MbxCode::NO_STORAGE, 0, 0);
  }

  // One response per loop and only on an idle transmitter: the next one waits until this one is on air.
  if (_out_count && _host.txIdle()) {
    Outgoing& o = _out[_out_head];
    _host.sendResponse(o.to.client, o.to.tag, o.body, o.len, o.to.path_return);
    _out_head = (_out_head + 1) % OUTQ;
    _out_count--;
  }
}

uint32_t MailboxCore::nextWakeupMillis(uint32_t millis_now) const {
  if (_out_count) return 0;
  uint32_t best = UINT32_MAX;
  for (auto& p : _pending) {
    if (!p.used) continue;
    uint32_t left = due(millis_now, p.deadline_ms) ? 0 : p.deadline_ms - millis_now;
    if (left < best) best = left;
  }
  return best;
}

}
