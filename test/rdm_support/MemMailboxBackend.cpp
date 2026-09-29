#include "MemMailboxBackend.h"

#include <SHA256.h>

#include <algorithm>
#include <string.h>

using namespace rdm;

namespace {

const uint8_t NO_PAYLOAD_FLAG = FETCH_FLAG_NO_PAYLOAD;

bool isLive(MemMailboxBackend::MsgState s) {
  return s == MemMailboxBackend::MsgState::STORED || s == MemMailboxBackend::MsgState::SENT ||
         s == MemMailboxBackend::MsgState::ON_RADIO;
}

MbxState wireState(MemMailboxBackend::MsgState s) {
  switch (s) {
    case MemMailboxBackend::MsgState::STORED:
    case MemMailboxBackend::MsgState::SENT:         return MbxState::STORED;
    case MemMailboxBackend::MsgState::ON_RADIO:     return MbxState::ON_RADIO;
    case MemMailboxBackend::MsgState::DELIVERED:    return MbxState::DELIVERED;
    case MemMailboxBackend::MsgState::REJECTED:     return MbxState::REJECTED;
    case MemMailboxBackend::MsgState::EXPIRED:      return MbxState::EXPIRED;
    case MemMailboxBackend::MsgState::SYNC_EXPIRED: return MbxState::SYNC_EXPIRED;
  }
  return MbxState::UNKNOWN;
}

void tokenFor(uint8_t out[8], const uint8_t k_owner[16], const uint8_t sender[32]) {
  SHA256 sha;
  uint8_t mac[32];
  sha.resetHMAC(k_owner, 16);
  sha.update(sender, 32);
  sha.finalizeHMAC(k_owner, 16, mac, sizeof(mac));
  memcpy(out, mac, 8);
}

}

bool MemMailboxBackend::accept() {
  _requests++;
  return _mode != Mode::NO_REPLY;
}

BackendReply& MemMailboxBackend::reply(BackendReply::Kind kind, uint32_t id, MbxCode code) {
  _replies.emplace_back();
  BackendReply& r = _replies.back();
  memset(&r, 0, sizeof(r));
  r.kind = kind;
  r.id = id;
  r.code = code;
  return r;
}

bool MemMailboxBackend::poll(BackendReply& out) {
  if (_replies.empty()) return false;
  out = _replies.front();
  _replies.pop_front();
  return true;
}

bool MemMailboxBackend::pollAcl(uint8_t pub_out[32], bool& is_owner, uint8_t owner_out[4]) {
  if (_acl_out.empty()) return false;
  const AclLine& a = _acl_out.front();
  memcpy(pub_out, a.pub, 32);
  is_owner = a.is_owner;
  memcpy(owner_out, a.owner, 4);
  _acl_out.pop_front();
  return true;
}

void MemMailboxBackend::housekeeping(uint32_t t) {
  // final_at is when the timer ran out, not when it was noticed, as in mbxd.
  for (auto& m : _msgs) {
    if ((m.state == MsgState::STORED || m.state == MsgState::SENT) && m.t_radio <= t) {
      m.state = MsgState::EXPIRED;
      m.payload.clear();
      m.final_at = m.t_radio;
      m.has_final = true;
    } else if (m.state == MsgState::ON_RADIO && m.t_sync <= t) {
      m.state = MsgState::SYNC_EXPIRED;
      m.payload.clear();
      m.final_at = m.t_sync;
      m.has_final = true;
    }
  }
  _msgs.erase(std::remove_if(_msgs.begin(), _msgs.end(),
                             [t](const Msg& m) { return m.has_final && (uint64_t)m.final_at + RETAIN_S <= t; }),
              _msgs.end());
}

void MemMailboxBackend::advance() { housekeeping(now()); }

MemMailboxBackend::Owner* MemMailboxBackend::ownerBy4(const uint8_t owner[4]) {
  for (auto& o : _owners) if (memcmp(o.pub, owner, 4) == 0) return &o;
  return nullptr;
}

MemMailboxBackend::Owner* MemMailboxBackend::ownerByPub(const uint8_t pub[32]) {
  for (auto& o : _owners) if (memcmp(o.pub, pub, 32) == 0) return &o;
  return nullptr;
}

MemMailboxBackend::Depositor* MemMailboxBackend::depositor(const uint8_t owner[4], const uint8_t pub[32]) {
  for (auto& d : _acl) if (memcmp(d.owner, owner, 4) == 0 && memcmp(d.pub, pub, 32) == 0) return &d;
  return nullptr;
}

bool MemMailboxBackend::isDenied(const uint8_t owner[4], const uint8_t pub[32]) const {
  for (auto& d : _deny) if (memcmp(d.owner, owner, 4) == 0 && memcmp(d.pub, pub, 32) == 0) return true;
  return false;
}

MemMailboxBackend::Msg* MemMailboxBackend::findMsg(const uint8_t pkt_hash[8]) {
  for (auto& m : _msgs) if (memcmp(m.pkt_hash, pkt_hash, 8) == 0) return &m;
  return nullptr;
}

bool MemMailboxBackend::store(uint32_t id, const uint8_t owner[4], const uint8_t sender_pub[32],
                              const uint8_t pkt_hash[8], const uint8_t* payload, uint8_t len) {
  if (!accept()) return true;
  uint32_t t = now();
  housekeeping(t);
  auto answer = [&](MbxCode code, uint32_t expires) {
    reply(BackendReply::Kind::STORE, id, code).expires = expires;
    return true;
  };
  if (len > MAX_INNER_PAYLOAD) return answer(MbxCode::TOO_BIG, 0);
  Owner* o = ownerBy4(owner);
  if (!o) return answer(MbxCode::UNKNOWN_OWNER, 0);
  Depositor* d = depositor(owner, sender_pub);
  if (isDenied(owner, sender_pub) || !d) return answer(MbxCode::NOT_AUTH, 0);
  if (Msg* m = findMsg(pkt_hash)) {
    if (memcmp(m->owner, owner, 4) == 0 && memcmp(m->sender, sender_pub, 32) == 0)
      return answer(MbxCode::ALREADY_STORED, m->t_radio);
    return answer(MbxCode::NOT_AUTH, 0);
  }
  size_t live = 0;
  for (auto& m : _msgs)
    if (memcmp(m.owner, owner, 4) == 0 && memcmp(m.sender, sender_pub, 32) == 0 && isLive(m.state)) live++;
  if (live >= o->quota) return answer(MbxCode::QUOTA, 0);

  Msg m;
  memcpy(m.pkt_hash, pkt_hash, 8);
  memcpy(m.owner, owner, 4);
  memcpy(m.sender, sender_pub, 32);
  m.payload.assign(payload, payload + len);
  m.created = t;
  m.t_radio = t + o->ttl_days * DAY_S;
  m.t_sync = 0;
  m.final_at = 0;
  m.has_final = false;
  m.state = MsgState::STORED;
  memset(m.ack_r, 0, 6);
  memset(m.ack_s, 0, 6);
  _msgs.push_back(m);
  d->last_seen = t;
  return answer(MbxCode::OK, m.t_radio);
}

bool MemMailboxBackend::reg(uint32_t id, const uint8_t sender_pub[32], const uint8_t owner[4], const uint8_t token[8]) {
  if (!accept()) return true;
  uint32_t t = now();
  housekeeping(t);
  Owner* o = ownerBy4(owner);
  if (!o) {
    reply(BackendReply::Kind::REG, id, MbxCode::UNKNOWN_OWNER);
    return true;
  }
  uint8_t expected[8];
  tokenFor(expected, o->k_owner, sender_pub);
  if (isDenied(owner, sender_pub) || memcmp(expected, token, 8) != 0) {
    reply(BackendReply::Kind::REG, id, MbxCode::NOT_AUTH);
    return true;
  }
  Depositor* d = depositor(owner, sender_pub);
  if (!d) {
    Depositor nd;
    memcpy(nd.pub, sender_pub, 32);
    memcpy(nd.owner, owner, 4);
    nd.seq = _acl_seq++;
    _acl.push_back(nd);
    d = &_acl.back();
  }
  d->last_seen = t;
  BackendReply& r = reply(BackendReply::Kind::REG, id, MbxCode::OK);
  r.ttl_days = o->ttl_days;
  r.quota = o->quota;
  return true;
}

void MemMailboxBackend::applyReport(uint32_t t, const Owner& o, const Report& r) {
  // Reports for unknown hashes, other owners' messages or transitions that do not fit are confirmed
  // anyway, otherwise Alice would repeat them forever.
  Msg* m = findMsg(r.pkt_hash);
  if (!m || memcmp(m->owner, o.pub, 4) != 0) return;
  bool fresh = m->state == MsgState::STORED || m->state == MsgState::SENT;
  switch (r.result) {
    case ReportResult::ON_RADIO:
    case ReportResult::DUPLICATE:
      if (fresh) {
        m->state = MsgState::ON_RADIO;
        memcpy(m->ack_r, r.ack, 6);
        m->t_sync = t + o.sync_days * DAY_S;
      }
      break;
    case ReportResult::SYNCED:
      if (fresh || m->state == MsgState::ON_RADIO || m->state == MsgState::SYNC_EXPIRED) {
        m->state = MsgState::DELIVERED;
        memcpy(m->ack_s, r.ack, 6);
        m->payload.clear();
        m->final_at = t;
        m->has_final = true;
      }
      break;
    case ReportResult::UNDECRYPTABLE:
      if (fresh) {
        m->state = MsgState::REJECTED;
        m->payload.clear();
        m->final_at = t;
        m->has_final = true;
      }
      break;
    case ReportResult::INBOX_FULL:
      if (m->state == MsgState::SENT) m->state = MsgState::STORED;
      break;
  }
}

// Oldest STORED copy of the next sender in pubkey order after the previous one, wrapping around, so one sender
// with a full quota does not hold up the others.
MemMailboxBackend::Msg* MemMailboxBackend::nextCopy(Owner& o) {
  const uint8_t* after = nullptr;   // smallest sender > last_sender
  const uint8_t* lowest = nullptr;
  for (auto& m : _msgs) {
    if (memcmp(m.owner, o.pub, 4) != 0 || m.state != MsgState::STORED) continue;
    if (!lowest || memcmp(m.sender, lowest, 32) < 0) lowest = m.sender;
    if (o.has_last_sender && memcmp(m.sender, o.last_sender, 32) > 0 && (!after || memcmp(m.sender, after, 32) < 0))
      after = m.sender;
  }
  const uint8_t* sender = after ? after : lowest;
  if (!sender) return nullptr;
  for (auto& m : _msgs)   // insertion order = oldest first
    if (memcmp(m.owner, o.pub, 4) == 0 && m.state == MsgState::STORED && memcmp(m.sender, sender, 32) == 0) return &m;
  return nullptr;
}

bool MemMailboxBackend::fetch(uint32_t id, const uint8_t client_pub[32], uint8_t flags, uint32_t store_id,
                              const Report* r, uint8_t n) {
  if (n > MAX_BATCH) return false;
  if (!accept()) return true;
  uint32_t t = now();
  housekeeping(t);
  Owner* o = ownerByPub(client_pub);
  if (!o) {
    reply(BackendReply::Kind::FETCH, id, MbxCode::NOT_AUTH);
    return true;
  }
  if (o->has_store_id && o->store_id != store_id) {
    // Alice's radio recreated its RDM storage: everything it may have lost goes out again (G12). T_radio
    // starts again: it is about reaching Alice's radio, which had already happened once.
    for (auto& m : _msgs) {
      if (memcmp(m.owner, o->pub, 4) == 0 && (m.state == MsgState::ON_RADIO || m.state == MsgState::SENT)) {
        m.state = MsgState::STORED;
        m.t_radio = t + o->ttl_days * DAY_S;
        memset(m.ack_r, 0, 6);
      }
    }
  }
  o->has_store_id = true;
  o->store_id = store_id;
  for (uint8_t i = 0; i < n; i++) applyReport(t, *o, r[i]);
  // A copy the next FETCH does not report on never reached Alice.
  for (auto& m : _msgs)
    if (memcmp(m.owner, o->pub, 4) == 0 && m.state == MsgState::SENT) m.state = MsgState::STORED;

  BackendReply& out = reply(BackendReply::Kind::FETCH, id, MbxCode::OK);
  out.reports_ok = n;
  bool withhold = _mode == Mode::WITHHOLD;
  if (!withhold && !(flags & NO_PAYLOAD_FLAG)) {
    if (Msg* m = nextCopy(*o)) {
      m->state = MsgState::SENT;
      out.inner_len = (uint8_t)m->payload.size();
      memcpy(out.inner, m->payload.data(), m->payload.size());
      memcpy(o->last_sender, m->sender, 32);
      o->has_last_sender = true;
    }
  }
  size_t remaining = 0;
  for (auto& m : _msgs)
    if (memcmp(m.owner, o->pub, 4) == 0 && m.state == MsgState::STORED) remaining++;
  out.remaining = withhold ? 0 : (uint8_t)std::min<size_t>(remaining, 255);
  return true;
}

bool MemMailboxBackend::stat(uint32_t id, const uint8_t sender_pub[32], const uint8_t (*hashes)[8], uint8_t n) {
  if (n == 0 || n > MAX_BATCH) return false;
  if (!accept()) return true;
  housekeeping(now());
  BackendReply& out = reply(BackendReply::Kind::STAT, id, MbxCode::OK);
  out.n = n;
  for (uint8_t i = 0; i < n; i++) {
    StatusReply& item = out.items[i];
    item.state = MbxState::UNKNOWN;
    memset(item.ack, 0, 6);
    Msg* m = findMsg(hashes[i]);
    if (!m || memcmp(m->sender, sender_pub, 32) != 0) continue;
    item.state = wireState(m->state);
    if (m->state == MsgState::ON_RADIO || m->state == MsgState::SYNC_EXPIRED) memcpy(item.ack, m->ack_r, 6);
    if (m->state == MsgState::DELIVERED) memcpy(item.ack, m->ack_s, 6);
    if (_lie != Lie::NONE && m->state != MsgState::DELIVERED) {
      item.state = MbxState::DELIVERED;
      if (_lie == Lie::DELIVERED_WITH_ACK_R) {
        memcpy(item.ack, m->ack_r, 6);
      } else {
        for (uint8_t k = 0; k < 6; k++) {
          _lie_rng = _lie_rng * 1664525u + 1013904223u;
          item.ack[k] = (uint8_t)(_lie_rng >> 24);
        }
      }
    }
  }
  return true;
}

bool MemMailboxBackend::addOwner(const uint8_t pub[32], const uint8_t k_owner[16], uint8_t ttl_days,
                                 uint8_t sync_days, uint8_t quota) {
  if (ttl_days < 1 || ttl_days > 30 || sync_days < 1 || quota < 1) return false;
  Owner* o = ownerBy4(pub);
  if (o && memcmp(o->pub, pub, 32) != 0) return false;
  if (!o) {
    _owners.emplace_back();
    o = &_owners.back();
    memcpy(o->pub, pub, 32);
    o->has_store_id = false;
    o->store_id = 0;
    o->has_last_sender = false;
  }
  memcpy(o->k_owner, k_owner, 16);
  o->ttl_days = ttl_days;
  o->sync_days = sync_days;
  o->quota = quota;
  return true;
}

bool MemMailboxBackend::deny(const uint8_t owner[4], const uint8_t pub[32]) {
  if (!ownerBy4(owner)) return false;
  if (!isDenied(owner, pub)) {
    Denied d;
    memcpy(d.owner, owner, 4);
    memcpy(d.pub, pub, 32);
    _deny.push_back(d);
  }
  _acl.erase(std::remove_if(_acl.begin(), _acl.end(),
                            [&](const Depositor& d) {
                              return memcmp(d.owner, owner, 4) == 0 && memcmp(d.pub, pub, 32) == 0;
                            }),
             _acl.end());
  return true;
}

void MemMailboxBackend::hello() {
  _acl_out.clear();
  for (auto& o : _owners) {
    if (_acl_out.size() >= ACL_MAX) break;
    AclLine a;
    memcpy(a.pub, o.pub, 32);
    a.is_owner = true;
    memcpy(a.owner, o.pub, 4);
    _acl_out.push_back(a);
  }
  std::vector<const Depositor*> recent;
  for (auto& d : _acl) if (!isDenied(d.owner, d.pub)) recent.push_back(&d);
  std::sort(recent.begin(), recent.end(), [](const Depositor* a, const Depositor* b) {
    return a->last_seen != b->last_seen ? a->last_seen > b->last_seen : a->seq > b->seq;
  });
  for (const Depositor* d : recent) {
    if (_acl_out.size() >= ACL_MAX) break;
    bool seen = false;
    for (auto& a : _acl_out) if (memcmp(a.pub, d->pub, 32) == 0) seen = true;
    if (seen) continue;
    AclLine a;
    memcpy(a.pub, d->pub, 32);
    a.is_owner = false;
    memcpy(a.owner, d->owner, 4);
    _acl_out.push_back(a);
  }
  _ready = true;
}

bool MemMailboxBackend::messageState(const uint8_t pkt_hash[8], MsgState& out) const {
  for (auto& m : _msgs) {
    if (memcmp(m.pkt_hash, pkt_hash, 8) == 0) {
      out = m.state;
      return true;
    }
  }
  return false;
}
