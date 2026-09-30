#include "RdmNode.h"

#include "RdmBytes.h"
#include "RdmCodec.h"
#include "RdmCrypto.h"

#include <string.h>

namespace rdm {

namespace {

const uint8_t FORMAT_VER = 1;

uint16_t scaled(uint16_t max, uint16_t min, uint64_t num, uint64_t den) {
  uint64_t v = (uint64_t)max * num / den;
  if (v < min) v = min;
  return (uint16_t)(v > max ? max : v);
}

// A parsed DM as the inbox takes it. Only a plain text (txt_type 0) that fits an inbox record is accepted.
bool toRecvInput(const codec::TxtParsed& p, const uint8_t* sender, RecvInput& out) {
  if (p.txt_type != 0 || p.text_len > INBOX_TEXT_MAX) return false;
  memset(&out, 0, sizeof(out));
  out.sender_pub = sender;
  out.ts = p.ts;
  out.flags = p.flags;
  out.txt_type = p.txt_type;
  out.text = p.text;
  out.text_len = (uint8_t)p.text_len;
  out.sender_cap = p.cap;
  return true;
}

}  // namespace

// Out-of-class definitions for C++11 builds (nRF52): Node binds these members to references.
constexpr uint16_t    Node::MBX_RECORD_SIZE;
constexpr const char* Node::MBX_PATH;

const Node::FileSpec Node::FILES[Node::N_FILES] = {
  { Clock::PATH,        Clock::RECORD_SIZE,        true,  1,                      1 },
  { ContactTable::PATH, ContactTable::RECORD_SIZE, true,  RDM_CONTACT_SLOTS_MAX,  8 },
  { Outbox::PATH,       Outbox::RECORD_SIZE,       true,  RDM_OUTBOX_SLOTS_MAX,   4 },
  { Outbox::WATCH_PATH, Outbox::WATCH_RECORD_SIZE, false, RDM_WATCH_SLOTS_MAX,    8 },
  { Inbox::PATH,        Inbox::RECORD_SIZE,        false, RDM_INBOX_SLOTS_MAX,    8 },
  { Inbox::REG_PATH,    Inbox::REG_RECORD_SIZE,    true,  RDM_REGISTER_SLOTS_MAX, 32 },
  { Node::MBX_PATH,     Node::MBX_RECORD_SIZE,     true,  1,                      1 },
};

// Slot count per file for one boot. A usable file already on flash keeps its slot count, so a later change in
// free space (other data growing) never migrates records away; its bytes count towards the budget. The others
// scale between min and max with the budget: files take at most half of the flash that is free for RDM
// (03 par. 6), the rest stays for contacts, prefs and blobs.
struct Node::StoragePlan {
  uint16_t slots[N_FILES];
  uint32_t existing_bytes;

  explicit StoragePlan(FileIO& io) : existing_bytes(0) {
    for (uint8_t i = 0; i < N_FILES; i++) slots[i] = existingSlots(io, FILES[i]);
  }

  void fit(uint32_t budget) {
    uint64_t full = 0;
    for (uint8_t i = 0; i < N_FILES; i++) full += RecordFile::fileSize(FILES[i].size, FILES[i].max, FILES[i].ab);
    uint64_t target = budget / 2;
    uint64_t num = target < full ? target : full;
    for (uint8_t i = 0; i < N_FILES; i++)
      if (slots[i] == 0) slots[i] = scaled(FILES[i].max, FILES[i].min, num, full);
  }

private:
  // 0: no usable file (missing, another record size). The one size() call also feeds existing_bytes.
  uint16_t existingSlots(FileIO& io, const FileSpec& f) {
    int32_t size = io.size(f.path);
    if (size > 0) existing_bytes += (uint32_t)size;
    uint16_t payload, slots;
    if (size < 16 || !detail::peekLayout(io, f.path, payload, slots) || payload != f.size) return 0;
    return slots > f.max ? f.max : slots;
  }
};

Node::Node(FileIO& io, NodeMeshHost& mesh, NodeAppSink& app) : _io(io), _mesh(mesh), _app(app) {
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
  for (uint8_t i = N_FILES; i-- > 0;) _files[i].reset();
}

// Below RDM_MIN_FREE_BYTES RDM stays off and the node behaves like upstream.
bool Node::begin() {
  shutdown();
  _n_hidden = 0;
  _has_own_mbx = false;
  _rtc_trusted = false;
  memset(_pending, 0, sizeof(_pending));
  memset(_gap, 0, sizeof(_gap));

  StoragePlan plan(_io);
  uint32_t budget = _io.freeBytes() + plan.existing_bytes;
  if (budget < RDM_MIN_FREE_BYTES) return false;
  plan.fit(budget);
  for (uint8_t i = 0; i < N_FILES; i++)
    _files[i].make(_io, FILES[i].path, FORMAT_VER, FILES[i].size, plan.slots[i], FILES[i].ab);

  ContactTable& contacts = _contacts.make(_files[F_CONTACTS].get());
  Clock& clock = _clock.make(_files[F_META].get());
  Inbox& inbox = _inbox.make(_files[F_INBOX].get(), _files[F_REG].get(), contacts, static_cast<InboxHost&>(*this));
  Outbox& outbox = _outbox.make(_files[F_OUTBOX].get(), _files[F_WATCH].get(), contacts, static_cast<OutboxHost&>(*this));
  Fetcher& fetcher = _fetcher.make(inbox, static_cast<FetcherHost&>(*this));

  bool recreated = false;
  if (!contacts.begin() || !inbox.begin(recreated) || !clock.begin(_mesh.millis(), recreated, _mesh.random32())) {
    shutdown();
    return false;
  }
  _ready = true;   // now() and the host callbacks below need the modules
  uint32_t t = now();
  if (!outbox.begin(t) || !loadOwnMailbox()) {
    shutdown();
    return false;
  }
  fetcher.begin(t, clock.storeId(), _mesh.random32());
  rebuildHiddenPeers();
  _mesh.onHiddenPeersChanged();   // K3: contacts loaded before begin() may include a mailbox
  return true;
}

bool Node::enabled() const { return _ready && _enabled; }

void Node::setEnabled(bool on) { _enabled = on; }

uint32_t Node::now() {
  bool trusted = false;
  uint32_t rtc = _app.rtcNow(trusted);
  Clock& clock = _clock.get();
  uint32_t ms = _mesh.millis();
  if (trusted && !_rtc_trusted) {
    // G6: the first trusted time since boot. Until now the clock counted on-time only (D1); what the outbox
    // timestamped since boot moves along with the jump, and the jump is persisted at once so the clock never
    // runs back past it after a power loss (in begin() the outbox is still empty: nothing to move).
    _rtc_trusted = true;
    uint32_t lagging = clock.now(ms, 0, false);
    _now = clock.now(ms, rtc, true);
    if (_now > lagging) _outbox.get().onClockJump(_now - lagging);
    clock.maybePersist(ms, true);
    return _now;
  }
  _now = clock.now(ms, rtc, trusted);
  return _now;
}

void Node::loop() {
  if (!enabled()) return;
  uint32_t t = now();
  _outbox.get().loop(t);
  _fetcher.get().loop(t);
  _clock.get().maybePersist(_mesh.millis(), false);
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

Node::AppSend Node::appSend(const uint8_t pub_prefix[6], uint32_t ts, uint8_t attempt, const char* text,
                            size_t text_len) {
  AppSend a;
  a.handled = false;
  a.sent = false;
  a.app_ack = 0;
  a.est_timeout_ms = 0;
  if (!enabled()) return a;

  uint32_t t = now();
  bool transmit = true;
  Outbox::SendResult r = _outbox.get().onAppSend(pub_prefix, ts, attempt, text, text_len, t, a.app_ack, transmit);
  if (r == Outbox::SendResult::NEW_ENTRY || r == Outbox::SendResult::EXISTING_ENTRY) {
    a.handled = true;
    // Entries in CUSTODY or ON_RADIO are not sent again; the DM carries the cap trailer (I8, the app's own retries
    // land on the same entry).
    if (!transmit) return a;
    uint8_t plain[5 + MAX_TEXT + 3];
    size_t n = codec::buildTxtPlain(plain, ts, 0, attempt, text, text_len, true);
    if (n && _mesh.sendTxtPlain(pub_prefix, plain, n, false, a.est_timeout_ms)) {
      a.sent = true;
      _outbox.get().onAppTransmitted(pub_prefix, ts, a.est_timeout_ms, now());
    }
    return a;
  }
  // K6 and TOO_BIG: the DM still goes out the upstream way, the client learns why it has no outbox entry
  uint8_t self[32], key[4];
  _mesh.selfPub(self);
  crypto::key(key, ts, text, text_len, self);
  _app.pushUserStatus(pub_prefix, a.app_ack, r == Outbox::SendResult::TOO_LONG ? UserStatus::TOO_BIG : UserStatus::NO_OUTBOX,
                      key, ts);
  return a;
}

// ---- receiving ----

RecvDecision Node::onPlainTxt(const uint8_t sender_pub[32], const uint8_t* data, size_t len, uint8_t path_len,
                              int8_t snr_x4) {
  RecvDecision d;
  memset(&d, 0, sizeof(d));
  d.result = RecvResult::STORE_ERROR;
  codec::TxtParsed p;
  RecvInput in;
  if (!enabled() || !codec::parseTxtPlain(data, len, p) || !toRecvInput(p, sender_pub, in)) return d;
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

  // The ACK covers the plaintext without block padding; parse accepted only zeros after it, so rebuilding
  // from the parsed fields gives exactly the sender's bytes.
  uint8_t plain[codec::CTRL_INFO_LEN], ack[4];
  size_t plain_len = codec::buildCtrlPlain(plain, ts, info);
  crypto::ctrlAck(ack, plain, plain_len, sender_pub);
  _mesh.sendAck(sender_pub, ack, 4);

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
  if (rebuildHiddenPeers()) _mesh.onHiddenPeersChanged();
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
    RecvInput in;
    if (!_mesh.decryptTxtPayload(inner, inner_len, sender, plain, plain_len) || !codec::parseTxtPlain(plain, plain_len, p) ||
        !toRecvInput(p, sender, in)) {
      ib.onUndecryptable(hash);
    } else {
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
  RecordFile& f = _files[F_MBX].get();
  if (f.open() == RecordFile::Open::FAILED) return false;
  uint8_t rec[MBX_RECORD_SIZE];
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

  RecordFile& f = _files[F_MBX].get();
  if (set) {
    uint8_t rec[MBX_RECORD_SIZE];
    memcpy(rec, mbx_pub, 32);
    memcpy(rec + 32, k_owner, 16);
    if (!f.write(0, rec)) return false;
    memcpy(_own_mbx, mbx_pub, 32);
    memcpy(_k_owner, k_owner, 16);
  } else if (!f.erase(0)) {
    return false;
  }
  _has_own_mbx = set;
  if (rebuildHiddenPeers()) _mesh.onHiddenPeersChanged();
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
    if (!_mesh.lookupContact(targets[k], pub, fav, path) || !fav) continue;
    uint8_t plain[codec::CTRL_INFO_LEN];
    size_t n = buildMbxCtrl(plain, pub);
    if (n) ob.addCtrl(targets[k], plain, n, t);
  }
  if (set) _fetcher.get().onReportQueued(t);   // first FETCH at the new mailbox soon, not after the interval
  return true;
}

uint8_t Node::listOutbox(OutEntry* out, uint8_t max, uint8_t offset) {
  if (!enabled()) return 0;
  Outbox& ob = _outbox.get();
  uint8_t n = 0;
  // get() succeeds for every index below count(); that guard also keeps offset + n from wrapping
  for (; n < max && offset + n < ob.count() && ob.get((uint8_t)(offset + n), out[n]); n++) {}
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
  return codec::buildCtrlPlain(out, _app.rtcNow(trusted), info);
}

void Node::maybeSendMbxInfo(const uint8_t sender_pub[32]) {
  if (!_has_own_mbx) return;
  uint8_t pub[32];
  bool fav = false, path = false;
  if (!_mesh.lookupContact(sender_pub, pub, fav, path) || !fav) return;
  const ContactRdm* c = _contacts.get().find(sender_pub);
  if (c && (c->flags & CR_MBX_INFO_SENT)) return;
  uint8_t plain[codec::CTRL_INFO_LEN];
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

uint32_t Node::reqTag() {
  bool trusted = false;
  return _clock.get().nextReqTimestamp(_app.rtcNow(trusted));
}

// Replies are matched on tag and peer; an old entry is overwritten after PENDING_MAX newer REQs, which is
// harmless: Outbox and Fetcher run their own time-outs and treat a missing reply like a lost one.
Node::Pending* Node::track(uint8_t kind, uint32_t tag, const uint8_t peer_pub[32], bool to_mailbox) {
  if (to_mailbox) noteReq(peer_pub);
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
  uint32_t tag = reqTag();
  if (!_mesh.sendReq(peer_pub, tag, body, len, flood, est_timeout_ms)) return false;
  Pending* p = track(kind, tag, peer_pub, to_mailbox);
  if (out) *out = p;
  return true;
}

// ---- OutboxHost ----

void Node::selfPub(uint8_t pub_out[32]) { _mesh.selfPub(pub_out); }

uint32_t Node::random32() { return _mesh.random32(); }

bool Node::contactPub(const uint8_t pub_prefix[6], uint8_t pub_out[32]) {
  bool fav = false, path = false;
  return _mesh.lookupContact(pub_prefix, pub_out, fav, path);
}

bool Node::hasDirectPath(const uint8_t pub_prefix[6]) {
  uint8_t pub[32];
  bool fav = false, path = false;
  return _mesh.lookupContact(pub_prefix, pub, fav, path) && path;
}

bool Node::txIdle() { return _mesh.txIdle(); }

bool Node::sendDm(const OutEntry& e, uint8_t attempt, bool flood, uint32_t& est_timeout_ms) {
  uint8_t plain[5 + MAX_TEXT + 3];
  size_t n = (e.flags & OF_CTRL) ? codec::ctrlPlainFromBody(plain, e.ts, (const uint8_t*)e.text, e.text_len)
                                 : codec::buildTxtPlain(plain, e.ts, 0, attempt, e.text, e.text_len, true);
  return n && _mesh.sendTxtPlain(e.pub_prefix, plain, n, flood, est_timeout_ms);
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
  if (!n || !_mesh.encryptTxtPayload(e.pub_prefix, plain, n, inner, inner_len) || inner_len > MAX_INNER_PAYLOAD) return false;
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
  uint32_t tag = reqTag();
  uint8_t plain[32];
  size_t len = codec::buildRegReq(plain, tag, pub_prefix, token);
  if (!len || !_mesh.sendAnonReq(mbx, plain, len, est_timeout_ms)) return false;
  memcpy(track(REQ_REGISTER, tag, mbx, true)->contact, pub_prefix, 6);
  return true;
}

bool Node::pushUserStatus(const OutEntry& e, UserStatus s) {
  return _app.pushUserStatus(e.pub_prefix, e.app_ack, s, e.key, e.ts);
}

bool Node::pushSendConfirmed(const OutEntry& e) { return _app.pushSendConfirmed(e.app_ack); }

// ---- InboxHost ----

void Node::onInboxChanged() { _app.pushMsgWaiting(); }

bool Node::sendAckS(const uint8_t sender_prefix[6], const uint8_t ack_s[6]) { return _mesh.sendAck(sender_prefix, ack_s, 6); }

void Node::onReportQueued() {
  if (_fetcher.live()) _fetcher.get().onReportQueued(_now);
}

// ---- FetcherHost ----

bool Node::ownMailbox(uint8_t mbx_pub_out[32]) {
  if (!_has_own_mbx) return false;
  memcpy(mbx_pub_out, _own_mbx, 32);
  return true;
}

bool Node::canFetch() { return _has_own_mbx && reqAllowed(_own_mbx); }

bool Node::sendFetch(uint8_t flags, uint32_t store_id, const Report* r, uint8_t n, uint32_t& est_timeout_ms) {
  if (!_has_own_mbx) return false;
  uint8_t body[1 + 1 + 4 + 1 + MAX_BATCH * 15];
  size_t len = codec::buildFetch(body, flags, store_id, r, n);
  return len && sendTracked(REQ_FETCH, _own_mbx, body, len, false, est_timeout_ms, nullptr);
}

}
