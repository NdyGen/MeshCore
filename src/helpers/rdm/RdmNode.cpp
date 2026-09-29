#include "RdmNode.h"

#include "RdmCodec.h"
#include "RdmCrypto.h"

#include <string.h>

namespace rdm {

namespace {

const char* const P_META     = "/rdm/meta";
const char* const P_CONTACTS = "/rdm/contacts";
const char* const P_OUTBOX   = "/rdm/outbox";
const char* const P_WATCH    = "/rdm/watch";
const char* const P_INBOX    = "/rdm/inbox";
const char* const P_REGISTER = "/rdm/register";
const char* const P_MBX      = "/rdm/mbx";

const uint16_t SZ_META = 12, SZ_CONTACT = 52, SZ_OUTBOX = 201, SZ_WATCH = 28, SZ_INBOX = 188, SZ_REG = 36;
const uint16_t SZ_MBX = 32 + 16;   // own mailbox: pubkey | K_owner
const uint8_t  FORMAT_VER = 1;

struct Plan { uint16_t outbox, watch, contacts, inbox, reg; };
const Plan PLAN_MAX = { RDM_OUTBOX_SLOTS_MAX, RDM_WATCH_SLOTS_MAX, RDM_CONTACT_SLOTS_MAX, RDM_INBOX_SLOTS_MAX,
                        RDM_REGISTER_SLOTS_MAX };
const Plan PLAN_MIN = { 4, 8, 8, 8, 32 };

uint32_t planBytes(const Plan& p) {
  return RecordFile::fileSize(SZ_OUTBOX, p.outbox, true) + RecordFile::fileSize(SZ_WATCH, p.watch, false) +
         RecordFile::fileSize(SZ_CONTACT, p.contacts, true) + RecordFile::fileSize(SZ_INBOX, p.inbox, false) +
         RecordFile::fileSize(SZ_REG, p.reg, true) + RecordFile::fileSize(SZ_META, 1, true) +
         RecordFile::fileSize(SZ_MBX, 1, true);
}

uint16_t scaled(uint16_t max, uint16_t min, uint64_t num, uint64_t den) {
  uint64_t v = (uint64_t)max * num / den;
  if (v < min) v = min;
  return (uint16_t)(v > max ? max : v);
}

// Slot count of an existing file with the same payload size, so a later change in free space (other data
// growing) never migrates records away. 0: no usable file.
uint16_t existingSlots(FileIO& io, const char* path, uint16_t payload, uint16_t max) {
  uint8_t h[16];
  if (io.size(path) < 16 || !io.read(path, 0, h, sizeof(h)) || memcmp(h, "RDMF", 4) != 0) return 0;
  if ((uint16_t)(h[6] | (h[7] << 8)) != payload) return 0;
  uint16_t slots = (uint16_t)(h[8] | (h[9] << 8));
  return slots > max ? max : slots;
}

uint32_t existingBytes(FileIO& io) {
  const char* const paths[] = { P_META, P_CONTACTS, P_OUTBOX, P_WATCH, P_INBOX, P_REGISTER, P_MBX };
  uint32_t n = 0;
  for (const char* p : paths) {
    int32_t s = io.size(p);
    if (s > 0) n += (uint32_t)s;
  }
  return n;
}

bool isZero(const uint8_t* p, size_t n) {
  for (size_t i = 0; i < n; i++)
    if (p[i]) return false;
  return true;
}

uint32_t get32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

}  // namespace

Node::Node(FileIO& io, NodeHost& host) : _io(io), _host(host) {
  memset(_own_mbx, 0, sizeof(_own_mbx));
  memset(_k_owner, 0, sizeof(_k_owner));
  memset(_hidden, 0, sizeof(_hidden));
  memset(_pending, 0, sizeof(_pending));
  memset(_gap, 0, sizeof(_gap));
}

void Node::shutdown() {
  _ready = false;
  _fetcher.reset();
  _outbox.reset();
  _inbox.reset();
  _clock.reset();
  _contacts.reset();
  _f_mbx.reset();
  _f_reg.reset();
  _f_inbox.reset();
  _f_watch.reset();
  _f_outbox.reset();
  _f_contacts.reset();
  _f_meta.reset();
}

// Files take at most half of the flash that is free for RDM (03 par. 6): the rest stays for contacts, prefs
// and blobs. Below RDM_MIN_FREE_BYTES RDM stays off and the node behaves like upstream.
bool Node::begin() {
  shutdown();
  _n_hidden = 0;
  _has_own_mbx = false;
  memset(_pending, 0, sizeof(_pending));
  memset(_gap, 0, sizeof(_gap));

  uint32_t budget = _io.freeBytes() + existingBytes(_io);
  if (budget < RDM_MIN_FREE_BYTES) return false;
  uint64_t full = planBytes(PLAN_MAX), target = budget / 2;
  uint64_t num = target < full ? target : full;
  Plan p;
  p.outbox   = scaled(PLAN_MAX.outbox, PLAN_MIN.outbox, num, full);
  p.watch    = scaled(PLAN_MAX.watch, PLAN_MIN.watch, num, full);
  p.contacts = scaled(PLAN_MAX.contacts, PLAN_MIN.contacts, num, full);
  p.inbox    = scaled(PLAN_MAX.inbox, PLAN_MIN.inbox, num, full);
  p.reg      = scaled(PLAN_MAX.reg, PLAN_MIN.reg, num, full);
  if (uint16_t s = existingSlots(_io, P_OUTBOX, SZ_OUTBOX, PLAN_MAX.outbox)) p.outbox = s;
  if (uint16_t s = existingSlots(_io, P_WATCH, SZ_WATCH, PLAN_MAX.watch)) p.watch = s;
  if (uint16_t s = existingSlots(_io, P_CONTACTS, SZ_CONTACT, PLAN_MAX.contacts)) p.contacts = s;
  if (uint16_t s = existingSlots(_io, P_INBOX, SZ_INBOX, PLAN_MAX.inbox)) p.inbox = s;
  if (uint16_t s = existingSlots(_io, P_REGISTER, SZ_REG, PLAN_MAX.reg)) p.reg = s;

  RecordFile& meta = _f_meta.make(_io, P_META, FORMAT_VER, SZ_META, (uint16_t)1, true);
  RecordFile& cf   = _f_contacts.make(_io, P_CONTACTS, FORMAT_VER, SZ_CONTACT, p.contacts, true);
  RecordFile& of   = _f_outbox.make(_io, P_OUTBOX, FORMAT_VER, SZ_OUTBOX, p.outbox, true);
  RecordFile& wf   = _f_watch.make(_io, P_WATCH, FORMAT_VER, SZ_WATCH, p.watch, false);
  RecordFile& inf  = _f_inbox.make(_io, P_INBOX, FORMAT_VER, SZ_INBOX, p.inbox, false);
  RecordFile& rf   = _f_reg.make(_io, P_REGISTER, FORMAT_VER, SZ_REG, p.reg, true);
  _f_mbx.make(_io, P_MBX, FORMAT_VER, SZ_MBX, (uint16_t)1, true);

  ContactTable& contacts = _contacts.make(cf);
  Clock& clock = _clock.make(meta);
  Inbox& inbox = _inbox.make(inf, rf, contacts, static_cast<InboxHost&>(*this));
  Outbox& outbox = _outbox.make(of, wf, contacts, static_cast<OutboxHost&>(*this));
  Fetcher& fetcher = _fetcher.make(inbox, static_cast<FetcherHost&>(*this));

  bool recreated = false;
  if (!contacts.begin() || !inbox.begin(recreated) || !clock.begin(_host.millis(), recreated, _host.random32())) {
    shutdown();
    return false;
  }
  _ready = true;   // now() and the host callbacks below need the modules
  uint32_t t = now();
  if (!outbox.begin(t) || !loadOwnMailbox()) {
    shutdown();
    return false;
  }
  fetcher.begin(t, clock.storeId(), _host.random32());
  rebuildHiddenPeers();
  _host.onHiddenPeersChanged();   // K3: contacts loaded before begin() may include a mailbox
  return true;
}

bool Node::enabled() const { return _ready && _enabled; }

void Node::setEnabled(bool on) { _enabled = on; }

uint32_t Node::now() {
  bool trusted = false;
  uint32_t rtc = _host.rtcNow(trusted);
  _now = _clock.get().now(_host.millis(), rtc, trusted);
  return _now;
}

void Node::loop() {
  if (!enabled()) return;
  uint32_t t = now();
  _outbox.get().loop(t);
  _fetcher.get().loop(t);
  _clock.get().maybePersist(_host.millis(), false);
}

uint32_t Node::nextWakeupMillis(uint32_t millis_now) const {
  if (!enabled()) return UINT32_MAX;
  const Clock& c = _clock.get();
  uint32_t w = c.millisUntil(_outbox.get().nextDue(_now), millis_now);
  uint32_t f = c.millisUntil(_fetcher.get().nextDue(_now), millis_now);
  if (f < w) w = f;
  int32_t p = (int32_t)(c.nextPersistMillis() - millis_now);
  uint32_t pw = p <= 0 ? 0 : (uint32_t)p;
  return pw < w ? pw : w;
}

// ---- sender ----

Node::AppSend Node::onAppSend(const uint8_t pub_prefix[6], uint32_t ts, uint8_t attempt, const char* text,
                              size_t text_len) {
  AppSend a;
  a.handled = false;
  a.transmit = true;
  a.cap_trailer = false;
  a.attempt = attempt;
  a.app_ack = 0;
  if (!enabled()) return a;

  uint32_t t = now();
  bool transmit = true;
  Outbox::SendResult r = _outbox.get().onAppSend(pub_prefix, ts, attempt, text, text_len, t, a.app_ack, transmit);
  if (r == Outbox::SendResult::NEW_ENTRY || r == Outbox::SendResult::EXISTING_ENTRY) {
    a.handled = true;
    a.transmit = transmit;
    a.cap_trailer = true;
    return a;
  }
  // K6 and TOO_BIG: the DM still goes out the upstream way, the client learns why it has no outbox entry
  uint8_t self[32], key[4];
  _host.selfPub(self);
  crypto::key(key, ts, text, text_len, self);
  _host.pushUserStatus(pub_prefix, a.app_ack, r == Outbox::SendResult::TOO_LONG ? UserStatus::TOO_BIG : UserStatus::NO_OUTBOX,
                       key, ts);
  return a;
}

void Node::onAppTransmitted(const uint8_t pub_prefix[6], uint32_t ts, uint32_t est_timeout_ms) {
  if (!enabled()) return;
  _outbox.get().onAppTransmitted(pub_prefix, ts, est_timeout_ms, now());
}

// ---- receiving ----

RecvDecision Node::onPlainTxt(const uint8_t sender_pub[32], const uint8_t* data, size_t len, uint8_t path_len,
                              int8_t snr_x4) {
  RecvDecision d;
  memset(&d, 0, sizeof(d));
  d.result = RecvResult::STORE_ERROR;
  codec::TxtParsed p;
  if (!enabled() || !codec::parseTxtPlain(data, len, p) || p.txt_type != 0 || p.text_len > INBOX_TEXT_MAX) return d;

  RecvInput in;
  memset(&in, 0, sizeof(in));
  in.sender_pub = sender_pub;
  in.ts = p.ts;
  in.flags = p.flags;
  in.txt_type = p.txt_type;
  in.text = p.text;
  in.text_len = (uint8_t)p.text_len;
  in.sender_cap = p.cap;
  in.path_len = path_len;
  in.snr_x4 = snr_x4;
  uint32_t t = now();
  d = _inbox.get().onMessage(in, t);
  _outbox.get().onHeard(sender_pub, p.cap, t);
  if (p.cap) maybeSendMbxInfo(sender_pub);
  return d;
}

void Node::onCtrlTxt(const uint8_t sender_pub[32], const uint8_t* data, size_t len) {
  uint32_t ts;
  codec::MbxInfo info;
  if (!enabled() || !codec::parseCtrlPlain(data, len, ts, info)) return;

  size_t plain_len = info.sub == CTRL_MBX_INFO ? 47 : 7;   // the ACK covers the plaintext without block padding
  uint8_t ack[4];
  crypto::ctrlAck(ack, data, plain_len, sender_pub);
  _host.sendAck(sender_pub, ack, 4);

  uint32_t t = now();
  _outbox.get().onHeard(sender_pub, true, t);   // only a fork sends CTRL
  ContactRdm* c = _contacts.get().findOrAdd(sender_pub);
  if (!c) return;
  ContactRdm old = *c;
  if (info.sub == CTRL_MBX_INFO) {
    if ((c->flags & CR_HAS_MBX) && memcmp(c->mbx_pub, info.mbx_pub, 32) == 0 && memcmp(c->token, info.token, 8) == 0) return;
    memcpy(c->mbx_pub, info.mbx_pub, 32);
    memcpy(c->token, info.token, 8);
    c->flags |= CR_HAS_MBX;
  } else {
    if (!(c->flags & CR_HAS_MBX)) return;
    c->flags &= (uint8_t)~CR_HAS_MBX;
  }
  if (!_contacts.get().save(*c)) {
    *c = old;
    return;
  }
  if (rebuildHiddenPeers()) _host.onHiddenPeersChanged();
  _outbox.get().onMailboxChanged(sender_pub, t);
}

bool Node::onAck(const uint8_t* ack, uint8_t len) {
  if (!enabled() || len < 4) return false;
  return _outbox.get().onAck(ack, len > 7 ? 7 : len, now());
}

uint8_t Node::onReceiptQuery(const uint8_t sender_pub[32], const uint8_t* body, size_t len, uint8_t* reply_body) {
  QueryItem q[MAX_BATCH];
  uint8_t n = 0;
  if (!enabled() || !codec::parseQuery(body, len, q, n)) return 0;
  QueryReply r[MAX_BATCH];
  for (uint8_t i = 0; i < n; i++) r[i] = _inbox.get().query(sender_pub, q[i]);
  return (uint8_t)codec::buildQueryResp(reply_body, r, n);
}

bool Node::onResponse(const uint8_t peer_pub[32], const uint8_t* data, size_t len) {
  if (!enabled() || len < 4) return false;
  uint32_t tag = get32(data);
  Pending* p = nullptr;
  for (uint8_t i = 0; i < PENDING_MAX && !p; i++)
    if (_pending[i].kind != REQ_NONE && _pending[i].tag == tag && memcmp(_pending[i].peer, peer_pub, 6) == 0) p = &_pending[i];
  if (!p) return false;

  Pending req = *p;
  p->kind = REQ_NONE;
  const uint8_t* body = data + 4;
  size_t blen = len - 4;
  uint32_t t = now();
  Outbox& ob = _outbox.get();
  switch (req.kind) {
    case REQ_QUERY: {
      QueryReply r[MAX_BATCH];
      uint8_t n = 0;
      if (codec::parseQueryResp(body, blen, r, n)) ob.onQueryReply(req.peer, req.asked.q, r, n < req.n ? n : req.n, t);
      break;
    }
    case REQ_DEPOSIT: {
      MbxCode st;
      uint8_t hash[8];
      uint32_t ttl;
      if (codec::parseDepositResp(body, blen, st, hash, ttl)) ob.onDepositReply(st, hash, ttl, t);
      break;
    }
    case REQ_STATUS: {
      StatusReply r[MAX_BATCH];
      uint8_t n = 0;
      if (codec::parseStatusResp(body, blen, r, n)) ob.onStatusReply(req.contact, req.asked.h, r, n < req.n ? n : req.n, t);
      break;
    }
    case REQ_REGISTER: {
      MbxCode st;
      uint8_t ttl_days, quota;
      uint32_t time_m;
      if (codec::parseRegResp(body, blen, st, ttl_days, quota, time_m)) ob.onRegisterReply(req.contact, st, t);
      break;
    }
    case REQ_FETCH:
      onFetchResponse(body, blen);
      break;
  }
  return true;
}

void Node::onFetchResponse(const uint8_t* body, size_t len) {
  uint8_t remaining, reports_ok, inner_len;
  const uint8_t* inner;
  if (!codec::parseFetchResp(body, len, remaining, reports_ok, inner, inner_len)) return;
  Inbox& ib = _inbox.get();
  if (inner_len) {
    uint8_t hash[8];
    crypto::txtPacketHash(hash, inner, inner_len);
    uint8_t sender[32], plain[MAX_INNER_PAYLOAD];
    size_t plain_len = sizeof(plain);
    codec::TxtParsed p;
    if (!_host.decryptTxtPayload(inner, inner_len, sender, plain, plain_len) || !codec::parseTxtPlain(plain, plain_len, p) ||
        p.txt_type != 0 || p.text_len > INBOX_TEXT_MAX) {
      ib.onUndecryptable(hash);
    } else {
      RecvInput in;
      memset(&in, 0, sizeof(in));
      in.sender_pub = sender;
      in.ts = p.ts;
      in.flags = p.flags;
      in.txt_type = p.txt_type;
      in.text = p.text;
      in.text_len = (uint8_t)p.text_len;
      in.sender_cap = p.cap;
      in.via_mailbox = true;
      in.mbx_hash = hash;
      in.path_len = 0xFF;
      ib.onMessage(in, _now);
    }
  }
  _fetcher.get().onFetchReply(remaining, reports_ok, inner_len != 0, _now);
}

void Node::onHeard(const uint8_t pub_prefix[6], bool had_cap_trailer) {
  if (enabled()) _outbox.get().onHeard(pub_prefix, had_cap_trailer, now());
}

void Node::onAdvert(const uint8_t pub[32]) {
  if (!enabled()) return;
  uint32_t t = now();
  if (_has_own_mbx && memcmp(_own_mbx, pub, 32) == 0) _fetcher.get().onMailboxAdvert(t);
  if (_contacts.get().findByMailbox(pub)) _outbox.get().onMailboxAdvert(pub, t);
}

uint8_t Node::hiddenPeerCount() const { return enabled() ? _n_hidden : 0; }

const uint8_t* Node::hiddenPeerPub(uint8_t i) const { return i < _n_hidden ? _hidden[i] : nullptr; }

// ---- app side ----

bool Node::nextInboxFrame(InRecord& out, uint16_t& slot) { return enabled() && _inbox.get().nextForApp(out, slot); }

void Node::onInboxFrameHanded(uint16_t slot) {
  if (enabled()) _inbox.get().onHandedToApp(slot);
}

void Node::onSyncRequest() {
  if (enabled()) _inbox.get().onNextSyncRequest(now());
}

void Node::onClientConnected(bool rdm_client) {
  if (enabled()) _outbox.get().onClientConnected(rdm_client, now());
}

void Node::onClientDisconnected() {
  if (enabled()) _inbox.get().onClientDisconnected();
}

bool Node::loadOwnMailbox() {
  RecordFile& f = _f_mbx.get();
  if (f.open() == RecordFile::Open::FAILED) return false;
  uint8_t rec[SZ_MBX];
  _has_own_mbx = f.read(0, rec) && !isZero(rec, 32);
  if (_has_own_mbx) {
    memcpy(_own_mbx, rec, 32);
    memcpy(_k_owner, rec + 32, 16);
  }
  return true;
}

// A new or dropped mailbox reaches the favourites that already learned the old one (G14); the others get
// MBX_INFO with their next fork DM.
bool Node::setOwnMailbox(const uint8_t* mbx_pub, const uint8_t* k_owner) {
  if (!_ready) return false;
  bool set = mbx_pub && k_owner;
  if (set && _has_own_mbx && memcmp(_own_mbx, mbx_pub, 32) == 0 && memcmp(_k_owner, k_owner, 16) == 0) return true;
  if (!set && !_has_own_mbx) return true;

  RecordFile& f = _f_mbx.get();
  if (set) {
    uint8_t rec[SZ_MBX];
    memcpy(rec, mbx_pub, 32);
    memcpy(rec + 32, k_owner, 16);
    if (!f.write(0, rec)) return false;
    memcpy(_own_mbx, mbx_pub, 32);
    memcpy(_k_owner, k_owner, 16);
  } else if (!f.erase(0)) {
    return false;
  }
  _has_own_mbx = set;
  if (rebuildHiddenPeers()) _host.onHiddenPeersChanged();
  if (!enabled()) return true;

  // Who must hear about it: contacts that acknowledged an MBX_INFO, and contacts with one still on its way (its
  // ACK not in yet), whose outbox entry the new message replaces; otherwise the stale one would arrive later.
  uint32_t t = now();
  uint8_t targets[RDM_CONTACT_SLOTS_MAX + RDM_OUTBOX_SLOTS_MAX][6];
  uint16_t n_targets = 0;
  auto addTarget = [&](const uint8_t* prefix) {
    for (uint16_t k = 0; k < n_targets; k++)
      if (memcmp(targets[k], prefix, 6) == 0) return;
    if (n_targets < sizeof(targets) / sizeof(targets[0])) memcpy(targets[n_targets++], prefix, 6);
  };
  ContactTable& ct = _contacts.get();
  for (uint16_t i = 0; i < ct.count(); i++) {
    const ContactRdm* c = ct.at(i);
    if (c && (c->flags & CR_MBX_INFO_SENT)) addTarget(c->pub_prefix);
  }
  Outbox& ob = _outbox.get();
  OutEntry e;
  for (uint8_t i = 0; i < ob.count(); i++)
    if (ob.get(i, e) && (e.flags & OF_CTRL) && !isFinal(e.state)) addTarget(e.pub_prefix);

  for (uint16_t k = 0; k < n_targets; k++) {
    uint8_t pub[32];
    bool fav = false, path = false;
    if (!_host.lookupContact(targets[k], pub, fav, path) || !fav) continue;
    uint8_t plain[47];
    size_t n = buildMbxCtrl(plain, pub);
    if (n) ob.addCtrl(targets[k], plain, n, t);
  }
  if (set) _fetcher.get().onReportQueued(t);   // first FETCH at the new mailbox soon, not after the interval
  return true;
}

uint8_t Node::listOutbox(OutEntry* out, uint8_t max, uint8_t offset) {
  if (!enabled()) return 0;
  Outbox& ob = _outbox.get();
  uint8_t n = 0, seen = 0;
  for (uint8_t i = 0; i < ob.count() && n < max; i++) {
    if (!ob.get(i, out[n])) continue;
    if (seen++ < offset) continue;
    n++;
  }
  return n;
}

// ---- helpers ----

size_t Node::buildMbxCtrl(uint8_t* out, const uint8_t contact_pub[32]) {
  codec::MbxInfo info;
  memset(&info, 0, sizeof(info));
  info.version = CTRL_VERSION;
  if (_has_own_mbx) {
    info.sub = CTRL_MBX_INFO;
    memcpy(info.mbx_pub, _own_mbx, 32);
    crypto::tokenB(info.token, _k_owner, contact_pub);
  } else {
    info.sub = CTRL_MBX_REVOKE;
  }
  bool trusted = false;
  return codec::buildCtrlPlain(out, _host.rtcNow(trusted), info);
}

void Node::maybeSendMbxInfo(const uint8_t sender_pub[32]) {
  if (!_has_own_mbx) return;
  uint8_t pub[32];
  bool fav = false, path = false;
  if (!_host.lookupContact(sender_pub, pub, fav, path) || !fav) return;
  const ContactRdm* c = _contacts.get().find(sender_pub);
  if (c && (c->flags & CR_MBX_INFO_SENT)) return;
  uint8_t plain[47];
  size_t n = buildMbxCtrl(plain, pub);
  if (n) _outbox.get().addCtrl(sender_pub, plain, n, _now);   // Outbox ignores a repeat of an unacknowledged one
}

bool Node::rebuildHiddenPeers() {
  uint8_t old[RDM_HIDDEN_PEERS][32];
  uint8_t n_old = _n_hidden;
  memcpy(old, _hidden, sizeof(old));
  _n_hidden = 0;
  if (_has_own_mbx) memcpy(_hidden[_n_hidden++], _own_mbx, 32);
  if (_contacts.live()) {
    ContactTable& ct = _contacts.get();
    for (uint16_t i = 0; i < ct.count() && _n_hidden < RDM_HIDDEN_PEERS; i++) {
      const ContactRdm* c = ct.at(i);
      if (!c || !(c->flags & CR_HAS_MBX)) continue;
      bool dup = false;
      for (uint8_t k = 0; k < _n_hidden && !dup; k++) dup = memcmp(_hidden[k], c->mbx_pub, 32) == 0;
      if (!dup) memcpy(_hidden[_n_hidden++], c->mbx_pub, 32);
    }
  }
  return _n_hidden != n_old || memcmp(old, _hidden, (size_t)_n_hidden * 32) != 0;
}

bool Node::contactMailbox(const uint8_t contact_prefix[6], uint8_t mbx_pub[32], uint8_t token[8]) {
  const ContactRdm* c = _contacts.get().find(contact_prefix);
  if (!c || !(c->flags & CR_HAS_MBX)) return false;
  memcpy(mbx_pub, c->mbx_pub, 32);
  if (token) memcpy(token, c->token, 8);
  return true;
}

// MailboxCore accepts one REQ per 5 s per client; Outbox and Fetcher share this gap per mailbox.
bool Node::reqAllowed(const uint8_t mbx_pub[32]) {
  for (auto& g : _gap)
    if (g.used && memcmp(g.prefix, mbx_pub, 6) == 0) return _now - g.last >= RDM_MBX_REQ_GAP_S;
  return true;
}

void Node::noteReq(const uint8_t mbx_pub[32]) {
  MbxGap* slot = nullptr;
  for (auto& g : _gap) {
    if (g.used && memcmp(g.prefix, mbx_pub, 6) == 0) { slot = &g; break; }
    if (!slot && !g.used) slot = &g;
  }
  if (!slot) slot = &_gap[0];
  memcpy(slot->prefix, mbx_pub, 6);
  slot->last = _now;
  slot->used = true;
}

// Replies are matched on tag and peer; an old entry is overwritten after PENDING_MAX newer REQs, which is
// harmless: Outbox and Fetcher run their own time-outs and treat a missing reply like a lost one.
Node::Pending* Node::addPending(uint8_t kind, uint32_t tag, const uint8_t peer_pub[32]) {
  Pending* p = &_pending[_next_pending];
  _next_pending = (uint8_t)((_next_pending + 1) % PENDING_MAX);
  memset(p, 0, sizeof(*p));
  p->kind = kind;
  p->tag = tag;
  memcpy(p->peer, peer_pub, 6);
  return p;
}

bool Node::sendTracked(uint8_t kind, const uint8_t peer_pub[32], const uint8_t* body, size_t len, bool flood,
                       uint32_t& est_timeout_ms, Pending** out) {
  bool to_mailbox = kind != REQ_QUERY;
  if (to_mailbox && !reqAllowed(peer_pub)) return false;
  bool trusted = false;
  uint32_t tag = _clock.get().nextReqTimestamp(_host.rtcNow(trusted));
  if (!_host.sendReq(peer_pub, tag, body, len, flood, est_timeout_ms)) return false;
  if (to_mailbox) noteReq(peer_pub);
  Pending* p = addPending(kind, tag, peer_pub);
  if (out) *out = p;
  return true;
}

// ---- OutboxHost ----

void Node::selfPub(uint8_t pub_out[32]) { _host.selfPub(pub_out); }

uint32_t Node::random32() { return _host.random32(); }

bool Node::contactPub(const uint8_t pub_prefix[6], uint8_t pub_out[32]) {
  bool fav = false, path = false;
  return _host.lookupContact(pub_prefix, pub_out, fav, path);
}

bool Node::hasDirectPath(const uint8_t pub_prefix[6]) {
  uint8_t pub[32];
  bool fav = false, path = false;
  return _host.lookupContact(pub_prefix, pub, fav, path) && path;
}

bool Node::txIdle() { return _host.txIdle(); }

bool Node::sendDm(const OutEntry& e, uint8_t attempt, bool flood, uint32_t& est_timeout_ms) {
  uint8_t plain[5 + MAX_TEXT + 3];
  size_t n;
  if (e.flags & OF_CTRL) {
    memcpy(plain, &e.ts, 4);
    plain[4] = TXT_TYPE_RDM_CTRL << 2;
    memcpy(plain + 5, e.text, e.text_len);
    n = 5 + e.text_len;
  } else {
    n = codec::buildTxtPlain(plain, e.ts, 0, attempt, e.text, e.text_len, true);
  }
  return n && _host.sendTxtPlain(e.pub_prefix, plain, n, flood, est_timeout_ms);
}

bool Node::sendReceiptQuery(const uint8_t pub_prefix[6], const QueryItem* q, uint8_t n, bool flood,
                            uint32_t& est_timeout_ms) {
  uint8_t pub[32], body[2 + MAX_BATCH * 8];
  if (n > MAX_BATCH || !contactPub(pub_prefix, pub)) return false;
  size_t len = codec::buildQuery(body, q, n);
  Pending* p = nullptr;
  if (!len || !sendTracked(REQ_QUERY, pub, body, len, flood, est_timeout_ms, &p)) return false;
  p->n = n;
  memcpy(p->asked.q, q, n * sizeof(QueryItem));
  return true;
}

bool Node::sendDeposit(const OutEntry& e, uint8_t attempt, uint8_t pkt_hash_out[8], uint32_t& est_timeout_ms) {
  uint8_t mbx[32];
  if ((e.flags & OF_CTRL) || !contactMailbox(e.pub_prefix, mbx, nullptr) || !reqAllowed(mbx)) return false;
  uint8_t plain[5 + MAX_TEXT + 3], inner[MAX_INNER_PAYLOAD + 16];
  size_t n = codec::buildTxtPlain(plain, e.ts, 0, attempt, e.text, e.text_len, true);
  size_t inner_len = sizeof(inner);
  if (!n || !_host.encryptTxtPayload(e.pub_prefix, plain, n, inner, inner_len) || inner_len > MAX_INNER_PAYLOAD) return false;
  crypto::txtPacketHash(pkt_hash_out, inner, inner_len);
  uint8_t body[1 + 4 + 1 + MAX_INNER_PAYLOAD];
  size_t len = codec::buildDeposit(body, e.pub_prefix, inner, (uint8_t)inner_len);
  Pending* p = nullptr;
  if (!len || !sendTracked(REQ_DEPOSIT, mbx, body, len, false, est_timeout_ms, &p)) return false;
  memcpy(p->contact, e.pub_prefix, 6);
  return true;
}

bool Node::sendStatus(const uint8_t pub_prefix[6], const uint8_t (*hashes)[8], uint8_t n, uint32_t& est_timeout_ms) {
  uint8_t mbx[32], body[2 + MAX_BATCH * 8];
  if (n > MAX_BATCH || !contactMailbox(pub_prefix, mbx, nullptr)) return false;
  size_t len = codec::buildStatus(body, hashes, n);
  Pending* p = nullptr;
  if (!len || !sendTracked(REQ_STATUS, mbx, body, len, false, est_timeout_ms, &p)) return false;
  memcpy(p->contact, pub_prefix, 6);
  p->n = n;
  memcpy(p->asked.h, hashes, n * 8);
  return true;
}

bool Node::sendRegister(const uint8_t pub_prefix[6], uint32_t& est_timeout_ms) {
  uint8_t mbx[32], token[8];
  if (!contactMailbox(pub_prefix, mbx, token) || !reqAllowed(mbx)) return false;
  bool trusted = false;
  uint32_t tag = _clock.get().nextReqTimestamp(_host.rtcNow(trusted));
  uint8_t plain[32];
  size_t len = codec::buildRegReq(plain, tag, pub_prefix, token);
  if (!len || !_host.sendAnonReq(mbx, plain, len, est_timeout_ms)) return false;
  noteReq(mbx);
  Pending* p = addPending(REQ_REGISTER, tag, mbx);
  memcpy(p->contact, pub_prefix, 6);
  return true;
}

bool Node::pushUserStatus(const OutEntry& e, UserStatus s) {
  return _host.pushUserStatus(e.pub_prefix, e.app_ack, s, e.key, e.ts);
}

bool Node::pushSendConfirmed(const OutEntry& e) { return _host.pushSendConfirmed(e.app_ack); }

// ---- InboxHost ----

void Node::onInboxChanged() { _host.pushMsgWaiting(); }

bool Node::sendAckS(const uint8_t sender_prefix[6], const uint8_t ack_s[6]) { return _host.sendAck(sender_prefix, ack_s, 6); }

void Node::onReportQueued() {
  if (_fetcher.live()) _fetcher.get().onReportQueued(_now);
}

// ---- FetcherHost ----

bool Node::ownMailbox(uint8_t mbx_pub_out[32]) {
  if (!_has_own_mbx) return false;
  memcpy(mbx_pub_out, _own_mbx, 32);
  return true;
}

bool Node::sendFetch(uint8_t flags, uint32_t store_id, const Report* r, uint8_t n, uint32_t& est_timeout_ms) {
  if (!_has_own_mbx) return false;
  uint8_t body[1 + 1 + 4 + 1 + MAX_BATCH * 15];
  size_t len = codec::buildFetch(body, flags, store_id, r, n);
  return len && sendTracked(REQ_FETCH, _own_mbx, body, len, false, est_timeout_ms, nullptr);
}

}
