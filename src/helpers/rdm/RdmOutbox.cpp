#include "RdmOutbox.h"

#include "RdmCodec.h"
#include "RdmCrypto.h"

#include <string.h>

#ifdef RDM_DEBUG
  #include <stdio.h>
  #define RDM_LOG(...) printf(__VA_ARGS__)
#else
  #define RDM_LOG(...) do {} while (0)
#endif

namespace rdm {

namespace {

const uint32_t NEVER       = 0xFFFFFFFFUL;
const uint16_t RECORD_SIZE = 201;
const uint16_t WATCH_SIZE  = 28;
const uint32_t RETRY_AFTER_LOSS_S = 86400;   // decision WP3: at least a day more after M or Alice lost the copy
const uint32_t T_MAX_CUSTODY_S = 30UL * 86400;   // G10: M's ttl_s, at most 30 days

// All outbox transmissions share one 60 s gap, so REQs to one mailbox are never closer than RDM_MBX_REQ_GAP_S.
static_assert(RDM_OUTBOX_TX_GAP_S >= RDM_MBX_REQ_GAP_S, "outbox gap must cover the mailbox REQ gap");
static_assert(RDM_OUTBOX_SLOTS_MAX <= 255 && RDM_WATCH_SLOTS_MAX <= 255, "slot indexes are 8 bit");

const uint32_t SCHED_DM[]       = RDM_SCHED_DM_BACKOFF;
const uint32_t SCHED_ON_RADIO[] = RDM_SCHED_ON_RADIO;
const uint32_t SCHED_STATUS[]   = RDM_SCHED_STATUS;
const uint32_t SCHED_REDEP[]    = RDM_SCHED_REDEPOSIT;

template <size_t N> uint32_t schedAt(const uint32_t (&s)[N], uint8_t step) {
  return s[step < N ? step : N - 1];
}

// Step whose interval is running after `elapsed` seconds on the schedule; resumes a schedule after a reboot.
template <size_t N> uint8_t stepFor(const uint32_t (&s)[N], uint32_t elapsed) {
  uint8_t k = 0;
  uint32_t cum = 0;
  while (k < N - 1 && cum + s[k] <= elapsed) cum += s[k++];
  return k;
}

uint32_t waitSecs(uint32_t est_timeout_ms) {                   // G3, G9: max(2 x est_timeout, 30 s)
  uint32_t s = (uint32_t)(((uint64_t)est_timeout_ms * 2 + 999) / 1000);
  return s > RDM_WAIT_ACK_MIN_S ? s : RDM_WAIT_ACK_MIN_S;
}

bool isDue(uint32_t t, uint32_t now) { return t != NEVER && t <= now; }
bool sinceAtLeast(uint32_t last, uint32_t now, uint32_t secs) { return last == NEVER || now - last >= secs; }
uint32_t minT(uint32_t a, uint32_t b) { return a < b ? a : b; }
uint32_t addT(uint32_t t, uint32_t d) { return t == NEVER || t > NEVER - d ? NEVER : t + d; }

void put32(uint8_t* p, uint32_t v) {
  p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
uint32_t get32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void encode(const OutEntry& e, uint8_t* b) {
  memcpy(b, e.pub_prefix, 6);
  put32(b + 6, e.ts);
  memcpy(b + 10, e.key, 4);
  b[14] = e.klass;
  b[15] = e.fw_attempt;
  b[16] = (uint8_t)e.state;
  b[17] = e.flags;
  put32(b + 18, e.app_ack);
  memcpy(b + 22, e.pkt_hash, 8);
  put32(b + 30, e.created);
  put32(b + 34, e.deadline);
  put32(b + 38, e.final_at);
  b[42] = e.text_len;
  memcpy(b + 43, e.text, MAX_TEXT + 1);
}

bool decode(const uint8_t* b, OutEntry& e) {
  if (b[16] > (uint8_t)OutState::SYNC_EXPIRED || b[42] > MAX_TEXT) return false;
  memcpy(e.pub_prefix, b, 6);
  e.ts = get32(b + 6);
  memcpy(e.key, b + 10, 4);
  e.klass = b[14];
  e.fw_attempt = b[15];
  e.state = (OutState)b[16];
  e.flags = b[17];
  e.app_ack = get32(b + 18);
  memcpy(e.pkt_hash, b + 22, 8);
  e.created = get32(b + 30);
  e.deadline = get32(b + 34);
  e.final_at = get32(b + 38);
  e.text_len = b[42];
  memcpy(e.text, b + 43, MAX_TEXT + 1);
  e.text[e.text_len] = 0;
  return true;
}

bool isZero(const uint8_t* p, size_t n) {
  for (size_t i = 0; i < n; i++) if (p[i]) return false;
  return true;
}

bool isCtrl(const OutEntry& e) { return (e.flags & OF_CTRL) != 0; }

bool beforeRadio(OutState s) { return s <= OutState::CUSTODY; }

}

Outbox::Outbox(RecordFile& file, RecordFile& watch, ContactTable& contacts, OutboxHost& host)
  : _file(file), _contacts(contacts), _host(host), _watch_file(watch), _n_slots(0), _n_watch(0), _tx_any(false),
    _last_fw_tx(0) {
  memset(_slots, 0, sizeof(_slots));
  memset(_peers, 0, sizeof(_peers));
  memset(_watch, 0, sizeof(_watch));
  memset(_self, 0, sizeof(_self));
}

// G16 first RETRY action, G6 boot kick and G17b repeat: 60-120 s
uint32_t Outbox::firstDelay() {
  return RDM_RETRY_FIRST_MIN_S + _host.random32() % (RDM_RETRY_FIRST_MAX_S - RDM_RETRY_FIRST_MIN_S + 1);
}

// G17a: every repeating interval gets its own random stretch, so timers of different nodes do not stay in step
uint32_t Outbox::jittered(uint32_t interval) {
  uint32_t j = interval / RDM_JITTER_DIV < RDM_JITTER_MAX_S ? interval / RDM_JITTER_DIV : RDM_JITTER_MAX_S;
  return interval + _host.random32() % (j + 1);
}

void Outbox::markDm(Slot& s, uint32_t now) {
  s.t_even = now + jittered(RDM_DM_EVEN_WITH_CAP_S);
}

bool Outbox::begin(uint32_t now) {
  _host.selfPub(_self);
  if (_file.open() == RecordFile::Open::FAILED || _file.payloadSize() != RECORD_SIZE) return false;
  if (_watch_file.open() == RecordFile::Open::FAILED || _watch_file.payloadSize() != WATCH_SIZE) return false;
  _n_slots = _file.slots() < RDM_OUTBOX_SLOTS_MAX ? (uint8_t)_file.slots() : (uint8_t)RDM_OUTBOX_SLOTS_MAX;
  _n_watch = _watch_file.slots() < RDM_WATCH_SLOTS_MAX ? (uint8_t)_watch_file.slots() : (uint8_t)RDM_WATCH_SLOTS_MAX;
  uint8_t wbuf[WATCH_SIZE];
  for (uint8_t k = 0; k < _n_watch; k++) {
    WatchIdx& w = _watch[k];
    w.used = _watch_file.read(k, wbuf);
    if (!w.used) continue;
    memcpy(w.ack_s, wbuf, 6);
    w.expires = get32(wbuf + 6);
    if (now >= w.expires) {
      w.used = false;
      _watch_file.erase(k);
    }
  }

  // G6: one query/probe/STATUS round 60-120 s after boot (same range as G16), then the normal schedules
  uint32_t kick = now + firstDelay();
  uint8_t buf[RECORD_SIZE];
  for (uint8_t i = 0; i < _n_slots; i++) {
    Slot& s = _slots[i];
    memset(&s, 0, sizeof(s));
    if (!_file.read(i, buf)) continue;
    if (!decode(buf, s.e)) {
      _file.erase(i);
      continue;
    }
    s.used = true;
    computeAcks(s);
    s.wait_until = s.t_dm = s.t_query = s.t_status = s.t_deposit = NEVER;
    s.w_status = s.w_query = s.r_status = s.r_query = NEVER;
    s.last_heard = s.last_advert = NEVER;
    markDm(s, now);
    s.req = REQ_NONE;

    OutEntry& e = s.e;
    uint32_t age = now - e.created;
    if (isFinal(e.state)) {
      // nothing scheduled
    } else if (isCtrl(e)) {
      e.state = OutState::RETRY;
      s.in_series = true;
      s.dm_step = stepFor(SCHED_DM, age);
      s.t_dm = kick;
    } else {
      switch (e.state) {
        case OutState::NEW:
        case OutState::WAIT_ACK:
          e.state = OutState::RETRY;
          // fall through
        case OutState::RETRY:
          s.in_series = true;
          s.dm_step = stepFor(SCHED_DM, age);
          if (hasCap(e.pub_prefix)) s.t_query = kick; else s.t_dm = kick;
          s.dep_step = stepFor(SCHED_REDEP, age);
          if (canDeposit(s)) s.t_deposit = now + jittered(schedAt(SCHED_REDEP, s.dep_step++));
          break;
        case OutState::DEPOSITING:
        case OutState::REGISTER:
          e.state = OutState::DEPOSITING;
          s.in_series = true;
          s.t_deposit = kick;
          break;
        case OutState::CUSTODY:
          s.status_step = stepFor(SCHED_STATUS, age);
          s.t_status = kick;
          s.t_query = now + jittered(RDM_CUSTODY_PROBE_S);
          break;
        case OutState::ON_RADIO: {
          uint32_t since = e.deadline >= RDM_T_SYNC_S ? e.deadline - RDM_T_SYNC_S : e.created;
          s.q_step = stepFor(SCHED_ON_RADIO, now - since);
          if (e.flags & OF_MBX_COPY) s.t_status = kick; else s.t_query = kick;
          break;
        }
        default:
          break;
      }
    }
    s.reported = (uint8_t)toUserStatus(e.state) + 1;
    persist(i);
  }
  for (uint8_t i = 0; i < _n_slots; i++) {
    if (_slots[i].used) releaseIfReported(i);
  }
  return true;
}

bool Outbox::persist(uint8_t i) {
  uint8_t buf[RECORD_SIZE];
  encode(_slots[i].e, buf);
  if (!_file.write(i, buf)) {
    RDM_LOG("rdm outbox: write of slot %u failed\n", (unsigned)i);
    return false;
  }
  return true;
}

void Outbox::release(uint8_t i) {
  _file.erase(i);
  _slots[i].used = false;
}

int Outbox::allocSlot() {
  for (uint8_t i = 0; i < _n_slots; i++) {
    if (!_slots[i].used) return i;
  }
  int oldest = -1;   // G2: full outbox frees the oldest final entry first, reported or not
  for (uint8_t i = 0; i < _n_slots; i++) {
    const OutEntry& e = _slots[i].e;
    if (isFinal(e.state) && (oldest < 0 || e.final_at < _slots[oldest].e.final_at)) oldest = i;
  }
  if (oldest < 0) return -1;
  release((uint8_t)oldest);
  return oldest;
}

void Outbox::computeAcks(Slot& s) {
  const OutEntry& e = s.e;
  if (isCtrl(e)) {
    uint8_t plain[5 + MAX_TEXT];
    put32(plain, e.ts);
    plain[4] = TXT_TYPE_RDM_CTRL << 2;
    memcpy(plain + 5, e.text, e.text_len);
    crypto::ctrlAck(s.ack_r[0], plain, 5 + e.text_len, _self);
    memset(s.ack_s, 0, sizeof(s.ack_s));
    return;
  }
  for (uint8_t c = 0; c < 4; c++) crypto::ackR(s.ack_r[c], e.ts, c, e.text, e.text_len, _self);
  crypto::ackS(s.ack_s, e.ts, 0, e.text, e.text_len, _self);
}

bool Outbox::matchAckR(const Slot& s, const uint8_t* ack) const {
  if (isCtrl(s.e)) return memcmp(ack, s.ack_r[0], 4) == 0;
  for (uint8_t c = 0; c < 4; c++) {
    if (memcmp(ack, s.ack_r[c], 4) == 0) return true;
  }
  return false;
}

int Outbox::findMsg(const uint8_t pub_prefix[6], uint32_t ts, const uint8_t key[4]) const {
  for (uint8_t i = 0; i < _n_slots; i++) {
    const Slot& s = _slots[i];
    if (s.used && !isCtrl(s.e) && s.e.ts == ts && memcmp(s.e.pub_prefix, pub_prefix, 6) == 0 &&
        memcmp(s.e.key, key, 4) == 0) return i;
  }
  return -1;
}

bool Outbox::hasCap(const uint8_t pub_prefix[6]) const {
  const ContactRdm* c = _contacts.find(pub_prefix);
  return c && (c->flags & CR_CAP);
}

void Outbox::setCap(const uint8_t pub_prefix[6], bool on) {
  ContactRdm* c = on ? _contacts.findOrAdd(pub_prefix) : _contacts.find(pub_prefix);
  if (!c || ((c->flags & CR_CAP) != 0) == on) return;
  if (on) c->flags |= CR_CAP; else c->flags &= ~CR_CAP;
  _contacts.save(*c);
}

bool Outbox::canDeposit(const Slot& s) const {
  if (isCtrl(s.e) || s.no_deposit || s.e.text_len > MAX_TEXT_MAILBOX) return false;
  const ContactRdm* c = _contacts.find(s.e.pub_prefix);
  return c && (c->flags & CR_HAS_MBX);
}

bool Outbox::mailboxMatches(const uint8_t pub_prefix[6], const uint8_t mbx_prefix[6]) const {
  const ContactRdm* c = _contacts.find(pub_prefix);
  return c && (c->flags & CR_HAS_MBX) && memcmp(c->mbx_pub, mbx_prefix, 6) == 0;
}

Outbox::Peer& Outbox::peer(const uint8_t pub_prefix[6]) {
  int free_idx = -1;
  for (uint8_t i = 0; i < RDM_OUTBOX_SLOTS_MAX; i++) {
    if (_peers[i].used && memcmp(_peers[i].prefix, pub_prefix, 6) == 0) return _peers[i];
    if (free_idx < 0) {
      bool referenced = false;
      for (uint8_t j = 0; _peers[i].used && j < _n_slots && !referenced; j++) {
        referenced = _slots[j].used && memcmp(_slots[j].e.pub_prefix, _peers[i].prefix, 6) == 0;
      }
      if (!referenced) free_idx = i;
    }
  }
  // at most one peer per slot is referenced, so an unreferenced one always exists
  Peer& p = _peers[free_idx < 0 ? 0 : free_idx];
  memset(&p, 0, sizeof(p));
  p.used = true;
  memcpy(p.prefix, pub_prefix, 6);
  return p;
}

void Outbox::resetDirectFails(const uint8_t pub_prefix[6]) {
  peer(pub_prefix).direct_fails = 0;
}

uint8_t Outbox::nextFwAttempt(Slot& s) {
  OutEntry& e = s.e;
  uint8_t a = e.fw_attempt;
  // 03 par. 1d: 252 + c descending by 4, above the app range 0-3 and keeping the ACK class; 63 variants, then wrap
  e.fw_attempt = a >= 8 ? (uint8_t)(a - 4) : (uint8_t)(FW_ATTEMPT_BASE + e.klass);
  return a;
}

// ---- state changes ----

void Outbox::setState(uint8_t i, OutState st, uint32_t now) {
  Slot& s = _slots[i];
  OutState old = s.e.state;
  if (old == st) return;
  s.e.state = st;
  s.w_status = s.w_query = s.r_status = s.r_query = NEVER;
  s.st_repeat = s.q_repeat = s.final_sent = false;
  if (isFinal(st)) {
    s.e.final_at = now;
    s.req = REQ_NONE;
    s.wait_until = s.t_dm = s.t_query = s.t_status = s.t_deposit = NEVER;
  }
  if (st == OutState::ON_RADIO || st == OutState::ON_RADIO_FINAL || st == OutState::DELIVERED) confirm(i);
  if (st == OutState::SYNC_EXPIRED) addWatch(s, now);
  persist(i);
  notify(i);
  if (isFinal(st)) releaseIfReported(i);
}

void Outbox::notify(uint8_t i) {
  Slot& s = _slots[i];
  if (isCtrl(s.e)) return;
  UserStatus us = toUserStatus(s.e.state);
  if (s.reported == (uint8_t)us + 1) return;
  s.reported = (uint8_t)us + 1;
  uint8_t flags = s.e.flags;
  if (_host.pushUserStatus(s.e, us)) s.e.flags &= ~OF_UNREPORTED; else s.e.flags |= OF_UNREPORTED;
  if (flags != s.e.flags) persist(i);
}

void Outbox::confirm(uint8_t i) {
  OutEntry& e = _slots[i].e;
  if (isCtrl(e) || (e.flags & OF_CONFIRM_SENT)) return;   // G8: once, at the first of ON_RADIO/DELIVERED
  if (_host.pushSendConfirmed(e)) {
    e.flags = (e.flags & ~OF_CONFIRM_PENDING) | OF_CONFIRM_SENT;
  } else {
    e.flags |= OF_CONFIRM_PENDING;
  }
}

void Outbox::releaseIfReported(uint8_t i) {
  const OutEntry& e = _slots[i].e;
  if (_slots[i].used && isFinal(e.state) && (isCtrl(e) || !(e.flags & OF_UNREPORTED))) release(i);
}

// Back before ON_RADIO: the CUSTODY (ttl_s) or ON_RADIO (T_sync) deadline no longer applies. T_radio again, but
// at least a day more, because M or Alice lost a copy that had already arrived (decision WP3).
void Outbox::leaveCopy(Slot& s, uint32_t now) {
  if (s.e.state != OutState::CUSTODY && s.e.state != OutState::ON_RADIO) return;
  uint32_t t_radio = addT(s.e.created, RDM_T_RADIO_S);
  uint32_t min_end = addT(now, RETRY_AFTER_LOSS_S);
  s.e.deadline = t_radio > min_end ? t_radio : min_end;
}

void Outbox::enterRetry(uint8_t i, uint32_t now, bool start_series) {
  Slot& s = _slots[i];
  s.req = REQ_NONE;
  s.wait_until = NEVER;
  leaveCopy(s, now);
  if (start_series && !s.in_series) {
    // G16: a WAIT_ACK timeout may be a lost ACK after delivery, so the first action waits 60-120 s
    s.in_series = true;
    s.dm_step = 0;
    uint32_t first = now + firstDelay();
    if (!isCtrl(s.e) && hasCap(s.e.pub_prefix)) {
      s.t_query = first;
    } else {
      s.t_dm = first;
    }
  }
  setState(i, OutState::RETRY, now);
}

void Outbox::enterOnRadio(uint8_t i, uint32_t now) {
  Slot& s = _slots[i];
  s.in_series = false;
  s.req = REQ_NONE;
  s.wait_until = s.t_dm = s.t_deposit = NEVER;
  s.q_step = 0;
  if (s.e.flags & OF_MBX_COPY) {   // G11
    s.t_status = now + jittered(SCHED_ON_RADIO[0]);
    s.t_query = NEVER;
  } else {
    s.t_query = now + jittered(SCHED_ON_RADIO[0]);
    s.t_status = NEVER;
  }
  s.e.deadline = now + RDM_T_SYNC_S;
  setState(i, OutState::ON_RADIO, now);
}

void Outbox::enterCustody(uint8_t i, uint32_t ttl_s, uint32_t now) {
  Slot& s = _slots[i];
  s.in_series = false;
  s.req = REQ_NONE;
  s.wait_until = s.t_dm = s.t_deposit = NEVER;
  s.dep_noreply = 0;
  s.dep_step = 0;
  s.status_step = 0;
  s.t_status = now + jittered(SCHED_STATUS[0]);
  s.t_query = now + jittered(RDM_CUSTODY_PROBE_S);
  s.e.flags |= OF_MBX_COPY;
  s.e.deadline = now + (ttl_s < T_MAX_CUSTODY_S ? ttl_s : T_MAX_CUSTODY_S) + RDM_CUSTODY_GRACE_S;   // G10
  setState(i, OutState::CUSTODY, now);
}

void Outbox::startDeposit(uint8_t i, bool new_round, uint32_t now) {
  Slot& s = _slots[i];
  leaveCopy(s, now);
  if (new_round) s.reg_done = false;
  s.req = REQ_NONE;
  s.wait_until = NEVER;
  s.t_deposit = now;
  setState(i, OutState::DEPOSITING, now);
}

void Outbox::depositFailed(uint8_t i, uint32_t now) {
  Slot& s = _slots[i];
  s.t_deposit = now + jittered(schedAt(SCHED_REDEP, s.dep_step));   // G13
  if (s.dep_step < 255) s.dep_step++;
  enterRetry(i, now);
}

void Outbox::onProofRadio(uint8_t i, bool cap, uint32_t now) {
  OutState st = _slots[i].e.state;
  if (beforeRadio(st)) {
    if (cap) enterOnRadio(i, now); else setState(i, OutState::ON_RADIO_FINAL, now);
  } else if (st == OutState::ON_RADIO && !cap) {
    setState(i, OutState::ON_RADIO_FINAL, now);   // G15: Alice went back to standard firmware
  }
}

void Outbox::onProofSynced(uint8_t i, uint32_t now) {
  if (_slots[i].e.state != OutState::DELIVERED) setState(i, OutState::DELIVERED, now);
}

void Outbox::onWaitTimeout(uint8_t i, uint32_t now) {
  Slot& s = _slots[i];
  s.wait_until = NEVER;
  if (!s.in_series && canDeposit(s)) {
    startDeposit(i, true, now);
  } else {
    enterRetry(i, now);
  }
}

void Outbox::onReqTimeout(uint8_t i, uint32_t now) {
  Slot& s = _slots[i];
  uint8_t req = s.req;
  s.req = REQ_NONE;
  s.wait_until = NEVER;
  if (req == REQ_DEP) {
    // G9: two DEPOSITs in a row without reply means M lost our key (cache miss): register first
    if (++s.dep_noreply >= 2 && !s.reg_done) {
      s.t_deposit = now;
      setState(i, OutState::REGISTER, now);
    } else {
      depositFailed(i, now);
    }
  } else if (req == REQ_REG) {
    depositFailed(i, now);
  }
}

// G17b: silence on a STATUS is more likely a collision than a dead mailbox, so repeat it once
void Outbox::onStatusTimeout(uint8_t i, uint32_t now) {
  Slot& s = _slots[i];
  s.w_status = NEVER;
  if (!s.st_repeat) {
    s.r_status = now + firstDelay();
  } else if (s.final_sent) {
    setState(i, OutState::EXPIRED, now);   // G10: M silent after the deadline, also on the repeat
  }
}

void Outbox::onQueryTimeout(uint8_t i, uint32_t now) {
  Slot& s = _slots[i];
  s.w_query = NEVER;
  if (!s.q_repeat) s.r_query = now + firstDelay();
}

// ---- app side ----

Outbox::SendResult Outbox::onAppSend(const uint8_t pub_prefix[6], uint32_t ts, uint8_t attempt, const char* text,
                                     size_t text_len, uint32_t now, uint32_t& app_ack_out, bool& transmit_out) {
  transmit_out = true;
  uint8_t ack[4];
  crypto::ackR(ack, ts, attempt & 3, text, text_len, _self);
  memcpy(&app_ack_out, ack, 4);   // same byte order as BaseChatMesh::composeMsgPacket's expected_ack
  if (text_len > MAX_TEXT) return SendResult::TOO_LONG;

  uint8_t key[4];
  crypto::key(key, ts, text, text_len, _self);
  int found = findMsg(pub_prefix, ts, key);
  if (found >= 0) {
    // 03 par. 1d: an app retry lands on the existing entry; the app matches SEND_CONFIRMED on its latest ack
    Slot& s = _slots[found];
    if (s.e.app_ack != app_ack_out) {
      s.e.app_ack = app_ack_out;
      persist((uint8_t)found);
    }
    OutState st = s.e.state;
    transmit_out = !(st == OutState::CUSTODY || st == OutState::ON_RADIO || isFinal(st));
    return SendResult::EXISTING_ENTRY;
  }

  int i = allocSlot();
  if (i < 0) return SendResult::NO_OUTBOX;
  Slot& s = _slots[i];
  memset(&s, 0, sizeof(s));
  OutEntry& e = s.e;
  memcpy(e.pub_prefix, pub_prefix, 6);
  e.ts = ts;
  memcpy(e.key, key, 4);
  e.klass = attempt & 3;
  e.fw_attempt = FW_ATTEMPT_BASE + e.klass;
  e.state = OutState::NEW;
  e.app_ack = app_ack_out;
  e.created = now;
  e.deadline = addT(now, RDM_T_RADIO_S);
  e.text_len = (uint8_t)text_len;
  memcpy(e.text, text, text_len);
  e.text[text_len] = 0;
  if (!persist((uint8_t)i)) {
    _file.erase((uint8_t)i);
    return SendResult::NO_OUTBOX;   // a promise we cannot keep across a reboot is not an outbox entry
  }
  s.used = true;
  computeAcks(s);
  s.wait_until = now + RDM_WAIT_ACK_MIN_S;   // the app normally follows with onAppTransmitted
  markDm(s, now);
  s.t_dm = s.t_query = s.t_status = s.t_deposit = NEVER;
  s.last_heard = s.last_advert = NEVER;
  s.w_status = s.w_query = s.r_status = s.r_query = NEVER;
  return SendResult::NEW_ENTRY;
}

void Outbox::onAppTransmitted(const uint8_t pub_prefix[6], uint32_t ts, uint32_t est_timeout_ms, uint32_t now) {
  int found = -1;
  for (uint8_t i = 0; i < _n_slots; i++) {
    const Slot& s = _slots[i];
    if (s.used && !isCtrl(s.e) && s.e.ts == ts && memcmp(s.e.pub_prefix, pub_prefix, 6) == 0 &&
        (found < 0 || !isFinal(s.e.state))) found = i;
  }
  if (found < 0) return;
  Slot& s = _slots[found];
  markDm(s, now);
  OutState st = s.e.state;
  if (st == OutState::NEW || st == OutState::WAIT_ACK || st == OutState::RETRY) {
    s.wait_until = now + waitSecs(est_timeout_ms);   // G3: every app retry restarts the timer
    setState((uint8_t)found, OutState::WAIT_ACK, now);
  }
  notify((uint8_t)found);   // G1: first QUEUED right after RESP_CODE_SENT
}

bool Outbox::addCtrl(const uint8_t pub_prefix[6], const uint8_t* ctrl_plain, size_t len, uint32_t now) {
  if (len < 7 || len - 5 > MAX_TEXT || ctrl_plain[4] != (TXT_TYPE_RDM_CTRL << 2)) return false;
  const uint8_t* body = ctrl_plain + 5;
  size_t body_len = len - 5;

  int i = -1;
  for (uint8_t j = 0; j < _n_slots; j++) {
    const Slot& s = _slots[j];
    if (s.used && isCtrl(s.e) && !isFinal(s.e.state) && memcmp(s.e.pub_prefix, pub_prefix, 6) == 0) {
      if (s.e.text_len == body_len && memcmp(s.e.text, body, body_len) == 0) return true;
      i = j;   // G14: the latest MBX_INFO/REVOKE replaces an unacknowledged one
    }
  }
  if (i < 0) i = allocSlot();
  if (i < 0) return false;

  Slot& s = _slots[i];
  memset(&s, 0, sizeof(s));
  OutEntry& e = s.e;
  memcpy(e.pub_prefix, pub_prefix, 6);
  e.ts = get32(ctrl_plain);
  e.state = OutState::NEW;
  e.flags = OF_CTRL;
  e.created = now;
  e.deadline = addT(now, RDM_T_RADIO_S);
  e.text_len = (uint8_t)body_len;
  memcpy(e.text, body, body_len);
  computeAcks(s);
  memcpy(&e.app_ack, s.ack_r[0], 4);
  if (!persist((uint8_t)i)) {
    _file.erase((uint8_t)i);
    return false;
  }
  s.used = true;
  s.wait_until = s.t_query = s.t_status = s.t_deposit = NEVER;
  s.last_heard = s.last_advert = NEVER;
  s.w_status = s.w_query = s.r_status = s.r_query = NEVER;
  markDm(s, now);
  s.t_dm = now;
  return true;
}

// ---- proofs and replies ----

bool Outbox::onAck(const uint8_t* ack, uint8_t len, uint32_t now) {
  if (len < 4) return false;
  if (len >= 6) {
    for (uint8_t i = 0; i < _n_slots; i++) {
      Slot& s = _slots[i];
      if (s.used && !isCtrl(s.e) && memcmp(ack, s.ack_s, 6) == 0) {
        setCap(s.e.pub_prefix, true);   // only a fork sends ACK_S
        resetDirectFails(s.e.pub_prefix);
        onProofSynced(i, now);
        return true;
      }
    }
    if (matchWatch(ack, now)) return true;
  }
  bool cap = codec::ackHasCap(ack, len);
  for (uint8_t i = 0; i < _n_slots; i++) {
    Slot& s = _slots[i];
    if (!s.used || !matchAckR(s, ack)) continue;
    resetDirectFails(s.e.pub_prefix);
    if (isCtrl(s.e)) {
      if (!isFinal(s.e.state)) {
        ContactRdm* c = _contacts.findOrAdd(s.e.pub_prefix);
        if (c) {
          uint8_t f = s.e.text[0] == CTRL_MBX_INFO ? (c->flags | CR_MBX_INFO_SENT) : (c->flags & ~CR_MBX_INFO_SENT);
          if (f != c->flags) {
            c->flags = f;
            _contacts.save(*c);
          }
        }
        setState(i, OutState::ON_RADIO_FINAL, now);
      }
      return true;
    }
    setCap(s.e.pub_prefix, cap);   // G15: an ACK without cap byte revokes the capability
    onProofRadio(i, cap, now);
    return true;
  }
  RDM_LOG("rdm outbox: ack without match ignored\n");
  return false;
}

void Outbox::onQueryReply(const uint8_t pub_prefix[6], const QueryItem* asked, const QueryReply* r, uint8_t n,
                          uint32_t now) {
  setCap(pub_prefix, true);   // a RECEIPT_QUERY answer comes from a fork (03 par. 1b)
  resetDirectFails(pub_prefix);
  for (uint8_t k = 0; k < n; k++) {
    int i = findMsg(pub_prefix, asked[k].ts, asked[k].key);
    if (i < 0) continue;
    Slot& s = _slots[i];
    OutState st = s.e.state;
    if (isFinal(st)) continue;
    s.w_query = s.r_query = NEVER;
    switch (r[k].state) {
      case QueryState::ON_RADIO:
        if (!matchAckR(s, r[k].ack)) {
          RDM_LOG("rdm outbox: query reply with invalid ACK_R ignored\n");
          break;
        }
        if (beforeRadio(st)) enterOnRadio((uint8_t)i, now);   // G4, G8
        break;
      case QueryState::SYNCED:
        if (memcmp(r[k].ack, s.ack_s, 6) != 0) {
          RDM_LOG("rdm outbox: query reply with invalid ACK_S ignored\n");
          break;
        }
        onProofSynced((uint8_t)i, now);
        break;
      case QueryState::EVICTED:
        if (st == OutState::ON_RADIO) {   // only synced records are evicted (I7)
          onProofSynced((uint8_t)i, now);
          break;
        }
        // Alice never had it
        // fall through
      case QueryState::UNKNOWN:
        if (st == OutState::ON_RADIO) {
          // Alice lost it: back to RETRY and the DM at once; a later WAIT_ACK timeout starts the series (G16)
          enterRetry((uint8_t)i, now, false);
          s.t_dm = now;
        } else if (st != OutState::NEW && st != OutState::WAIT_ACK) {
          s.t_dm = now;   // I4: Alice is reachable, deliver directly even while M holds a copy
        }
        break;
    }
  }
}

void Outbox::onDepositReply(MbxCode st, const uint8_t pkt_hash[8], uint32_t ttl_s, uint32_t now) {
  // matched on pkt_hash, which M echoes (also with an error code) and only the depositor can predict
  if (isZero(pkt_hash, 8)) return;
  for (uint8_t i = 0; i < _n_slots; i++) {
    Slot& s = _slots[i];
    if (!s.used || isCtrl(s.e) || memcmp(s.e.pkt_hash, pkt_hash, 8) != 0) continue;
    OutState state = s.e.state;
    bool waiting = state == OutState::DEPOSITING && s.req == REQ_DEP;
    if (st == MbxCode::OK || st == MbxCode::ALREADY_STORED) {
      // a late STORED after we gave up still means M holds the copy
      if (waiting || state == OutState::RETRY) enterCustody(i, ttl_s, now);
      return;
    }
    if (!waiting) return;
    s.dep_noreply = 0;
    s.req = REQ_NONE;
    s.wait_until = NEVER;
    if (st == MbxCode::NOT_AUTH && !s.reg_done) {
      s.t_deposit = now;
      setState(i, OutState::REGISTER, now);
    } else {
      if (st == MbxCode::TOO_BIG) s.no_deposit = true;
      depositFailed(i, now);
    }
    return;
  }
  RDM_LOG("rdm outbox: deposit reply without matching pkt_hash ignored\n");
}

void Outbox::onStatusReply(const uint8_t pub_prefix[6], const uint8_t (*asked)[8], const StatusReply* r, uint8_t n,
                           uint32_t now) {
  for (uint8_t k = 0; k < n; k++) {
    for (uint8_t i = 0; i < _n_slots; i++) {
      Slot& s = _slots[i];
      if (!s.used || isCtrl(s.e) || !(s.e.flags & OF_MBX_COPY) || memcmp(s.e.pub_prefix, pub_prefix, 6) != 0 ||
          memcmp(s.e.pkt_hash, asked[k], 8) != 0) continue;
      OutState st = s.e.state;
      const StatusReply& rep = r[k];
      s.w_status = s.r_status = NEVER;
      if (st == OutState::ON_RADIO) {
        if (rep.state == MbxState::DELIVERED && memcmp(rep.ack, s.ack_s, 6) == 0) onProofSynced(i, now);
        break;
      }
      if (st != OutState::CUSTODY) break;
      bool final_check = s.final_sent || now >= s.e.deadline;
      switch (rep.state) {
        case MbxState::ON_RADIO:
          if (matchAckR(s, rep.ack)) enterOnRadio(i, now);
          else RDM_LOG("rdm outbox: STATUS with invalid ACK_R ignored\n");
          break;
        case MbxState::DELIVERED:
          if (memcmp(rep.ack, s.ack_s, 6) == 0) onProofSynced(i, now);   // G8
          else RDM_LOG("rdm outbox: STATUS with invalid ACK_S ignored\n");
          break;
        case MbxState::REJECTED:
          setState(i, OutState::REJECTED, now);
          break;
        case MbxState::UNKNOWN:
          if (final_check) {
            setState(i, OutState::EXPIRED, now);
          } else {
            s.e.flags &= ~OF_MBX_COPY;
            s.dep_step = 0;
            s.t_deposit = now + jittered(schedAt(SCHED_REDEP, s.dep_step++));
            enterRetry(i, now);
          }
          break;
        case MbxState::SYNC_EXPIRED:
          if (matchAckR(s, rep.ack)) {
            setState(i, OutState::SYNC_EXPIRED, now);
            break;
          }
          // fall through
        case MbxState::EXPIRED:
          if (final_check) setState(i, OutState::EXPIRED, now);   // G10: only after expires + grace
          break;
        case MbxState::STORED:
          if (final_check) {
            s.e.deadline = now + RDM_CUSTODY_GRACE_S;   // M still keeps it past its own expiry: look again later
            s.final_sent = false;
            persist(i);
          }
          break;
      }
      break;
    }
  }
}

void Outbox::onRegisterReply(const uint8_t pub_prefix[6], MbxCode st, uint32_t now) {
  for (uint8_t i = 0; i < _n_slots; i++) {
    Slot& s = _slots[i];
    if (!s.used || s.e.state != OutState::REGISTER || s.req != REQ_REG ||
        memcmp(s.e.pub_prefix, pub_prefix, 6) != 0) continue;
    s.req = REQ_NONE;
    s.wait_until = NEVER;
    s.dep_noreply = 0;
    if (st == MbxCode::OK) startDeposit(i, false, now); else depositFailed(i, now);
  }
}

void Outbox::onHeard(const uint8_t pub_prefix[6], bool had_cap_trailer, uint32_t now) {
  setCap(pub_prefix, had_cap_trailer);   // G15
  for (uint8_t i = 0; i < _n_slots; i++) {
    Slot& s = _slots[i];
    if (!s.used || isFinal(s.e.state) || memcmp(s.e.pub_prefix, pub_prefix, 6) != 0) continue;
    if (!sinceAtLeast(s.last_heard, now, RDM_HEARD_MIN_S)) continue;
    OutState st = s.e.state;
    if (st == OutState::RETRY) {
      s.t_dm = now;   // G15: hearing Alice means she is on air now, send the whole DM
    } else if (st == OutState::ON_RADIO || st == OutState::CUSTODY) {
      s.t_query = now;
    } else {
      continue;
    }
    s.last_heard = now;
  }
}

void Outbox::onMailboxAdvert(const uint8_t mbx_pub_prefix[6], uint32_t now) {
  for (uint8_t i = 0; i < _n_slots; i++) {
    Slot& s = _slots[i];
    if (!s.used || isCtrl(s.e) || !mailboxMatches(s.e.pub_prefix, mbx_pub_prefix)) continue;
    if (!sinceAtLeast(s.last_advert, now, RDM_MBX_ADVERT_MIN_S)) continue;
    OutState st = s.e.state;
    if (st == OutState::CUSTODY) {
      s.t_status = now;
    } else if (st == OutState::RETRY && canDeposit(s)) {
      s.t_deposit = now;   // G13
    } else {
      continue;
    }
    s.last_advert = now;
  }
}

void Outbox::onMailboxChanged(const uint8_t pub_prefix[6], uint32_t now) {
  const ContactRdm* c = _contacts.find(pub_prefix);
  bool has_mbx = c && (c->flags & CR_HAS_MBX);
  for (uint8_t i = 0; i < _n_slots; i++) {
    Slot& s = _slots[i];
    if (!s.used || isCtrl(s.e) || isFinal(s.e.state) || memcmp(s.e.pub_prefix, pub_prefix, 6) != 0) continue;
    OutState st = s.e.state;
    // G14: a copy at the old M is caught by Alice's register if she still fetches it
    s.dep_attempt_valid = false;
    memset(s.e.pkt_hash, 0, sizeof(s.e.pkt_hash));
    s.no_deposit = false;
    s.dep_noreply = 0;
    s.dep_step = 0;
    if (st == OutState::CUSTODY || st == OutState::DEPOSITING || st == OutState::REGISTER) {
      s.e.flags &= ~OF_MBX_COPY;
      if (has_mbx) {
        s.in_series = true;
        startDeposit(i, true, now);
      } else {
        s.t_deposit = NEVER;
        enterRetry(i, now);
      }
    } else if (st == OutState::RETRY) {
      s.t_deposit = has_mbx ? now : NEVER;
      persist(i);
    } else if (st == OutState::ON_RADIO && (s.e.flags & OF_MBX_COPY)) {
      s.e.flags &= ~OF_MBX_COPY;   // the old M is no longer asked; Alice answers queries herself
      s.t_status = NEVER;
      s.t_query = now + jittered(schedAt(SCHED_ON_RADIO, s.q_step));
      persist(i);
    }
  }
}

void Outbox::onClientConnected(bool rdm_client, uint32_t now) {
  (void)now;
  for (uint8_t i = 0; i < _n_slots; i++) {
    Slot& s = _slots[i];
    if (!s.used || isCtrl(s.e)) continue;
    uint8_t flags = s.e.flags;
    if ((s.e.flags & OF_CONFIRM_PENDING) && _host.pushSendConfirmed(s.e)) {
      s.e.flags = (s.e.flags & ~OF_CONFIRM_PENDING) | OF_CONFIRM_SENT;
    }
    if (rdm_client && (s.e.flags & OF_UNREPORTED) && _host.pushUserStatus(s.e, toUserStatus(s.e.state))) {
      s.e.flags &= ~OF_UNREPORTED;
    }
    if (flags != s.e.flags) persist(i);
    releaseIfReported(i);
  }
}

// ---- receipt watch ----
// Records in /rdm/watch: ack_s(6) | expires(4) | pub_prefix(6) | ts(4) | K(4) | app_ack(4); RAM holds ack_s and expires.

void Outbox::addWatch(const Slot& s, uint32_t now) {
  if (_n_watch == 0) return;
  uint8_t idx = 0;
  for (uint8_t k = 0; k < _n_watch; k++) {
    if (!_watch[k].used) {
      idx = k;
      break;
    }
    if (_watch[k].expires < _watch[idx].expires) idx = k;
  }
  uint8_t buf[WATCH_SIZE];
  uint32_t expires = addT(now, RDM_WATCH_S);
  memcpy(buf, s.ack_s, 6);
  put32(buf + 6, expires);
  memcpy(buf + 10, s.e.pub_prefix, 6);
  put32(buf + 16, s.e.ts);
  memcpy(buf + 20, s.e.key, 4);
  put32(buf + 24, s.e.app_ack);
  if (!_watch_file.write(idx, buf)) {
    RDM_LOG("rdm outbox: watch write failed\n");
    return;
  }
  WatchIdx& w = _watch[idx];
  w.used = true;
  memcpy(w.ack_s, s.ack_s, 6);
  w.expires = expires;
}

bool Outbox::matchWatch(const uint8_t* ack_s, uint32_t now) {
  for (uint8_t k = 0; k < _n_watch; k++) {
    WatchIdx& w = _watch[k];
    if (!w.used || memcmp(w.ack_s, ack_s, 6) != 0) continue;
    uint8_t buf[WATCH_SIZE];
    bool have = _watch_file.read(k, buf);
    w.used = false;
    _watch_file.erase(k);
    if (now >= w.expires) return false;
    if (have) {
      OutEntry e;
      memset(&e, 0, sizeof(e));
      memcpy(e.pub_prefix, buf + 10, 6);
      e.ts = get32(buf + 16);
      memcpy(e.key, buf + 20, 4);
      e.app_ack = get32(buf + 24);
      e.state = OutState::DELIVERED;
      e.final_at = now;
      _host.pushUserStatus(e, UserStatus::DELIVERED);
    }
    return true;
  }
  return false;
}

// ---- scheduling ----

uint32_t Outbox::actionDue(uint8_t i, Action a) const {
  const Slot& s = _slots[i];
  if (!s.used) return NEVER;
  const OutEntry& e = s.e;
  OutState st = e.state;
  switch (a) {
    case ACT_DM:
      if (isCtrl(e)) return st == OutState::NEW || st == OutState::RETRY ? s.t_dm : NEVER;
      if (st == OutState::RETRY) {
        // G15: with a known cap still a whole DM every 24 h, in case Alice went back to standard firmware
        return hasCap(e.pub_prefix) ? minT(s.t_dm, s.t_even) : s.t_dm;
      }
      return st == OutState::CUSTODY || st == OutState::DEPOSITING || st == OutState::REGISTER ? s.t_dm : NEVER;
    case ACT_QUERY:
      if (isCtrl(e)) return NEVER;
      if (st == OutState::RETRY) return hasCap(e.pub_prefix) ? s.t_query : NEVER;
      if (st == OutState::ON_RADIO) return s.w_query != NEVER ? NEVER : minT(s.t_query, s.r_query);
      return st == OutState::CUSTODY ? s.t_query : NEVER;
    case ACT_STATUS:
      if (s.w_status != NEVER) return NEVER;
      if (st == OutState::CUSTODY) return minT(minT(s.t_status, s.r_status), s.final_sent ? NEVER : e.deadline);
      return st == OutState::ON_RADIO && (e.flags & OF_MBX_COPY) ? minT(s.t_status, s.r_status) : NEVER;
    case ACT_DEPOSIT:
      if (st == OutState::DEPOSITING) return s.req == REQ_NONE ? s.t_deposit : NEVER;
      return st == OutState::RETRY && canDeposit(s) ? s.t_deposit : NEVER;
    case ACT_REGISTER:
      return st == OutState::REGISTER && s.req == REQ_NONE ? s.t_deposit : NEVER;
    default:
      return NEVER;
  }
}

uint32_t Outbox::eventDue(uint8_t i) const {
  const Slot& s = _slots[i];
  if (!s.used) return NEVER;
  const OutEntry& e = s.e;
  OutState st = e.state;
  if (isFinal(st)) return addT(e.final_at, RDM_FINAL_KEEP_S);
  uint32_t t = s.reported == 0 && !isCtrl(e) ? 0 : NEVER;
  if (st == OutState::NEW || st == OutState::WAIT_ACK || s.req != REQ_NONE) t = minT(t, s.wait_until);
  if (st == OutState::CUSTODY || st == OutState::ON_RADIO) t = minT(t, minT(s.w_status, s.w_query));
  if (st != OutState::CUSTODY) t = minT(t, e.deadline);   // CUSTODY ends through the final STATUS (G10)
  return t;
}

uint32_t Outbox::gapFree() const {
  return _tx_any ? _last_fw_tx + RDM_OUTBOX_TX_GAP_S : 0;
}

uint32_t Outbox::nextDue(uint32_t now) const {
  uint32_t t = NEVER;
  uint32_t send = NEVER;
  for (uint8_t i = 0; i < _n_slots; i++) {
    t = minT(t, eventDue(i));
    for (uint8_t a = 0; a < ACT_COUNT; a++) send = minT(send, actionDue(i, (Action)a));
  }
  for (uint8_t k = 0; k < _n_watch; k++) {
    if (_watch[k].used) t = minT(t, _watch[k].expires);
  }
  if (send != NEVER) {
    uint32_t gap = gapFree();
    t = minT(t, send > gap ? send : gap);
  }
  return t == NEVER ? NEVER : (t > now ? t : now);
}

void Outbox::loop(uint32_t now) {
  for (uint8_t i = 0; i < _n_slots; i++) {
    Slot& s = _slots[i];
    if (!s.used) continue;
    OutState st = s.e.state;
    if (isFinal(st)) {
      if (now >= addT(s.e.final_at, RDM_FINAL_KEEP_S)) release(i);   // G2, default D3
      continue;
    }
    if (s.reported == 0) notify(i);
    if (st != OutState::CUSTODY && now >= s.e.deadline) {
      setState(i, st == OutState::ON_RADIO ? OutState::SYNC_EXPIRED : OutState::EXPIRED, now);
      continue;
    }
    if ((st == OutState::NEW || st == OutState::WAIT_ACK) && isDue(s.wait_until, now)) {
      onWaitTimeout(i, now);
    } else if (s.req != REQ_NONE && isDue(s.wait_until, now)) {
      onReqTimeout(i, now);
    }
    if ((st == OutState::CUSTODY || st == OutState::ON_RADIO) && isDue(s.w_status, now)) {
      onStatusTimeout(i, now);
      if (!s.used || s.e.state != st) continue;
    }
    if (st == OutState::ON_RADIO && isDue(s.w_query, now)) onQueryTimeout(i, now);
  }
  for (uint8_t k = 0; k < _n_watch; k++) {
    if (_watch[k].used && now >= _watch[k].expires) {
      _watch[k].used = false;
      _watch_file.erase(k);
    }
  }

  if (now < gapFree() || !_host.txIdle()) return;   // one outbox transmission per 60 s, only on an idle TX queue
  // every action that does not transmit moves its own schedule past `now`, so this terminates
  for (int guard = 0; guard < RDM_OUTBOX_SLOTS_MAX * ACT_COUNT * 2; guard++) {
    int best = -1;
    Action best_a = ACT_DM;
    uint32_t best_t = NEVER;
    for (uint8_t i = 0; i < _n_slots; i++) {
      for (uint8_t a = 0; a < ACT_COUNT; a++) {
        uint32_t t = actionDue(i, (Action)a);
        if (t <= now && t < best_t) {
          best_t = t;
          best = i;
          best_a = (Action)a;
        }
      }
    }
    if (best < 0) return;
    if (runAction((uint8_t)best, best_a, now)) {
      _tx_any = true;
      _last_fw_tx = now;
      return;
    }
  }
}

bool Outbox::runAction(uint8_t i, Action a, uint32_t now) {
  switch (a) {
    case ACT_DM:       return runDm(i, now);
    case ACT_QUERY:    return runQuery(i, now);
    case ACT_STATUS:   return runStatus(i, now);
    case ACT_DEPOSIT:  return runDeposit(i, now);
    case ACT_REGISTER: return runRegister(i, now);
    default:           return false;
  }
}

bool Outbox::runDm(uint8_t i, uint32_t now) {
  Slot& s = _slots[i];
  OutEntry& e = s.e;
  bool retry = e.state == OutState::RETRY || e.state == OutState::NEW;
  bool backoff = retry && !isCtrl(e) && !hasCap(e.pub_prefix);
  markDm(s, now);
  if (backoff || isCtrl(e)) {
    s.t_dm = now + jittered(schedAt(SCHED_DM, s.dm_step));
    if (s.dm_step < 255) s.dm_step++;
  } else {
    s.t_dm = NEVER;
  }

  uint8_t pub[32];
  if (!_host.contactPub(e.pub_prefix, pub)) {
    RDM_LOG("rdm outbox: contact gone, DM skipped\n");
    return false;
  }
  // 03 par. 1e (L7): after 2 unanswered direct attempts one flood, at most once per 4 h per contact
  Peer& p = peer(e.pub_prefix);
  bool direct = _host.hasDirectPath(e.pub_prefix);
  bool flood_ok = !p.flooded || now - p.last_flood >= RDM_PROBE_FLOOD_MIN_S;
  bool flood;
  if (direct && p.direct_fails < 2) flood = false;
  else if (flood_ok) flood = true;
  else if (direct) flood = false;
  else return false;

  uint8_t attempt = isCtrl(e) ? 0 : nextFwAttempt(s);
  uint32_t est = 0;
  bool sent = _host.sendDm(e, attempt, flood, est);
  persist(i);   // fw_attempt moved on, also when not sent, so no attempt value is ever reused soon
  if (!sent) return false;
  if (flood) {
    p.flooded = true;
    p.last_flood = now;
    p.direct_fails = 0;
  } else if (p.direct_fails < 255) {
    p.direct_fails++;
  }
  if (retry) {
    s.wait_until = now + waitSecs(est);
    setState(i, OutState::WAIT_ACK, now);
  }
  return true;
}

bool Outbox::runQuery(uint8_t i, uint32_t now) {
  const uint8_t* prefix = _slots[i].e.pub_prefix;
  QueryItem items[MAX_BATCH];
  uint8_t idx[MAX_BATCH];
  uint8_t n = 0;
  // bundled per contact (03 par. 5): every entry of this contact whose query is due
  for (uint8_t j = 0; j < _n_slots && n < MAX_BATCH; j++) {
    Slot& s = _slots[j];
    if (!s.used || memcmp(s.e.pub_prefix, prefix, 6) != 0 || actionDue(j, ACT_QUERY) > now) continue;
    items[n].ts = s.e.ts;
    memcpy(items[n].key, s.e.key, 4);
    idx[n++] = j;
    OutState st = s.e.state;
    if (st == OutState::RETRY) {
      s.t_query = now + jittered(now - s.e.created < 86400 ? RDM_SCHED_PROBE_DAY1 : RDM_SCHED_PROBE_LATER);
    } else if (st == OutState::CUSTODY) {
      s.t_query = now + jittered(RDM_CUSTODY_PROBE_S);
    } else {
      // ON_RADIO; the G17b repeat keeps the schedule of the original query
      s.q_repeat = s.t_query > now;
      s.r_query = NEVER;
      if (s.q_repeat) {
        // schedule stays
      } else if (s.e.flags & OF_MBX_COPY) {
        s.t_query = NEVER;   // G11: only on hearing Alice
      } else {
        if (s.q_step < 255) s.q_step++;
        s.t_query = now + jittered(schedAt(SCHED_ON_RADIO, s.q_step));
      }
    }
  }

  uint8_t pub[32];
  if (!_host.contactPub(prefix, pub)) return false;
  Peer& p = peer(prefix);
  bool direct = _host.hasDirectPath(prefix);
  bool flood_ok = !p.flooded || now - p.last_flood >= RDM_PROBE_FLOOD_MIN_S;
  bool flood;
  if (direct && p.direct_fails < 2) flood = false;
  else if (flood_ok) flood = true;
  else if (direct) flood = false;
  else return false;

  uint32_t est = 0;
  if (!_host.sendReceiptQuery(prefix, items, n, flood, est)) return false;
  if (flood) {
    p.flooded = true;
    p.last_flood = now;
    p.direct_fails = 0;
  } else if (p.direct_fails < 255) {
    p.direct_fails++;
  }
  // G17b applies to queries in ON_RADIO only; in RETRY and CUSTODY silence is the normal case (Alice offline)
  for (uint8_t k = 0; k < n; k++) {
    Slot& s = _slots[idx[k]];
    if (s.e.state == OutState::ON_RADIO) s.w_query = now + waitSecs(est);
  }
  return true;
}

bool Outbox::runStatus(uint8_t i, uint32_t now) {
  const uint8_t* prefix = _slots[i].e.pub_prefix;
  uint8_t hashes[MAX_BATCH][8];
  uint8_t idx[MAX_BATCH];
  bool final[MAX_BATCH];
  uint8_t n = 0;
  for (uint8_t j = 0; j < _n_slots && n < MAX_BATCH; j++) {
    Slot& s = _slots[j];
    if (!s.used || memcmp(s.e.pub_prefix, prefix, 6) != 0 || actionDue(j, ACT_STATUS) > now) continue;
    memcpy(hashes[n], s.e.pkt_hash, 8);
    bool custody = s.e.state == OutState::CUSTODY;
    final[n] = custody && now >= s.e.deadline;
    idx[n++] = j;
    bool scheduled = s.t_status <= now || (custody && !s.final_sent && now >= s.e.deadline);
    s.st_repeat = !scheduled;   // G17b: the repeat keeps the schedule of the original STATUS
    s.r_status = NEVER;
    if (!scheduled) continue;
    if (custody) {
      if (s.status_step < 255) s.status_step++;
      s.t_status = now + jittered(schedAt(SCHED_STATUS, s.status_step));
    } else {
      if (s.q_step < 255) s.q_step++;
      s.t_status = now + jittered(schedAt(SCHED_ON_RADIO, s.q_step));
    }
  }
  uint32_t est = 0;
  bool sent = _host.sendStatus(prefix, hashes, n, est);
  for (uint8_t k = 0; k < n; k++) {
    Slot& s = _slots[idx[k]];
    if (sent) {
      s.w_status = now + waitSecs(est);
      if (final[k]) s.final_sent = true;   // G10: the check after ttl_s + grace decides
    } else if (final[k]) {
      setState(idx[k], OutState::EXPIRED, now);
    }
  }
  return sent;
}

bool Outbox::runDeposit(uint8_t i, uint32_t now) {
  Slot& s = _slots[i];
  if (s.e.state == OutState::RETRY) startDeposit(i, true, now);
  s.t_deposit = NEVER;
  uint8_t attempt = s.dep_attempt_valid ? s.dep_attempt : nextFwAttempt(s);
  uint8_t hash[8];
  uint32_t est = 0;
  if (!_host.sendDeposit(s.e, attempt, hash, est)) {
    depositFailed(i, now);
    return false;
  }
  s.dep_attempt = attempt;
  s.dep_attempt_valid = true;
  memcpy(s.e.pkt_hash, hash, 8);
  s.req = REQ_DEP;
  s.wait_until = now + waitSecs(est);
  persist(i);
  return true;
}

bool Outbox::runRegister(uint8_t i, uint32_t now) {
  Slot& s = _slots[i];
  s.t_deposit = NEVER;
  s.reg_done = true;
  uint32_t est = 0;
  if (!_host.sendRegister(s.e.pub_prefix, est)) {
    depositFailed(i, now);
    return false;
  }
  s.req = REQ_REG;
  s.wait_until = now + waitSecs(est);
  return true;
}

// ---- listing ----

uint8_t Outbox::count() const {
  uint8_t n = 0;
  for (uint8_t i = 0; i < _n_slots; i++) {
    if (_slots[i].used) n++;
  }
  return n;
}

bool Outbox::get(uint8_t idx, OutEntry& out) const {
  for (uint8_t i = 0; i < _n_slots; i++) {
    if (!_slots[i].used) continue;
    if (idx-- == 0) {
      out = _slots[i].e;
      return true;
    }
  }
  return false;
}

}
