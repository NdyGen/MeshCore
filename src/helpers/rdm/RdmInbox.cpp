#include "RdmInbox.h"

#include "RdmBytes.h"
#include "RdmCrypto.h"

#include <string.h>

namespace rdm {

namespace {

constexpr uint16_t NO_SLOT  = 0xFFFF;
constexpr uint8_t  REG_ON_RADIO = 0;
constexpr uint8_t  REG_SYNCED   = 1;

// RAM index flags
constexpr uint8_t IX_USED    = 0x01;
constexpr uint8_t IX_SYNCED  = 0x02;      // register
constexpr uint8_t IX_PENDING = 0x04;      // register
constexpr uint8_t IX_OFFERED = 0x02;      // inbox: handed to the app on this connection

void packIn(const InRecord& r, uint8_t* p) {
  memcpy(p, r.sender_prefix, 6);
  put32(p + 6, r.ts);
  put32(p + 10, r.recv_time);
  p[14] = r.txt_type;
  p[15] = r.path_len;
  p[16] = (uint8_t)r.snr_x4;
  p[17] = r.flags;
  memcpy(p + 18, r.mbx_hash, 8);
  p[26] = r.text_len;
  memcpy(p + 27, r.text, INBOX_TEXT_MAX + 1);
}

void unpackIn(const uint8_t* p, InRecord& r) {
  memcpy(r.sender_prefix, p, 6);
  r.ts = get32(p + 6);
  r.recv_time = get32(p + 10);
  r.txt_type = p[14];
  r.path_len = p[15];
  r.snr_x4 = (int8_t)p[16];
  r.flags = p[17];
  memcpy(r.mbx_hash, p + 18, 8);
  r.text_len = p[26] > INBOX_TEXT_MAX ? (uint8_t)INBOX_TEXT_MAX : p[26];
  memcpy(r.text, p + 27, INBOX_TEXT_MAX + 1);
  r.text[r.text_len] = 0;
}

void packReg(const RegRecord& r, uint8_t* p) {
  memcpy(p, r.sender_prefix, 6);
  put32(p + 6, r.ts);
  memcpy(p + 10, r.key, 4);
  memcpy(p + 14, r.ack_r, 4);
  memcpy(p + 18, r.ack_s, 6);
  p[24] = r.state;
  p[25] = r.flags;
  memcpy(p + 26, r.mbx_hash, 8);
  put16(p + 34, r.inbox_slot);
}

void unpackReg(const uint8_t* p, RegRecord& r) {
  memcpy(r.sender_prefix, p, 6);
  r.ts = get32(p + 6);
  memcpy(r.key, p + 10, 4);
  memcpy(r.ack_r, p + 14, 4);
  memcpy(r.ack_s, p + 18, 6);
  r.state = p[24];
  r.flags = p[25];
  memcpy(r.mbx_hash, p + 26, 8);
  r.inbox_slot = get16(p + 34);
}

bool samePrefix(const uint8_t* a, const uint8_t* b) { return memcmp(a, b, 6) == 0; }

Report reportOf(const RegRecord& r) {
  Report rep;
  memcpy(rep.pkt_hash, r.mbx_hash, 8);
  memset(rep.ack, 0, sizeof(rep.ack));
  if (r.state == REG_SYNCED) {
    rep.result = ReportResult::SYNCED;
    memcpy(rep.ack, r.ack_s, 6);
  } else {
    rep.result = (r.flags & RF_REPORT_DUP) ? ReportResult::DUPLICATE : ReportResult::ON_RADIO;
    memcpy(rep.ack, r.ack_r, 4);
  }
  return rep;
}

uint8_t regIdxFlags(const RegRecord& r) {
  return IX_USED | (r.state == REG_SYNCED ? IX_SYNCED : 0) | ((r.flags & RF_REPORT_PENDING) ? IX_PENDING : 0);
}

}  // namespace

// Out-of-class definitions for C++11 builds (nRF52): Node binds these members to references.
constexpr uint16_t    Inbox::RECORD_SIZE;
constexpr const char* Inbox::PATH;
constexpr uint16_t    Inbox::REG_RECORD_SIZE;
constexpr const char* Inbox::REG_PATH;

Inbox::Inbox(RecordFile& inbox, RecordFile& reg, ContactTable& contacts, InboxHost& host)
    : _inbox(inbox), _reg(reg), _contacts(contacts), _host(host), _n_extra(0), _reg_slots(0), _in_slots(0),
      _handed(NO_SLOT) {
  memset(_reg_idx, 0, sizeof(_reg_idx));
  memset(_in_idx, 0, sizeof(_in_idx));
}

bool Inbox::begin(bool& recreated_out) {
  recreated_out = false;
  _reg_slots = _in_slots = 0;
  _n_extra = 0;
  _handed = NO_SLOT;
  memset(_reg_idx, 0, sizeof(_reg_idx));
  memset(_in_idx, 0, sizeof(_in_idx));
  if (_inbox.payloadSize() != RECORD_SIZE || _reg.payloadSize() != REG_RECORD_SIZE) return false;

  RecordFile::Open oi = _inbox.open();
  RecordFile::Open orr = _reg.open();
  if (oi == RecordFile::Open::FAILED || orr == RecordFile::Open::FAILED) return false;
  recreated_out = oi == RecordFile::Open::CREATED || orr == RecordFile::Open::CREATED;
  if (orr == RecordFile::Open::CREATED) resetWatermarks();
  _in_slots = _inbox.slots() < RDM_INBOX_SLOTS_MAX ? _inbox.slots() : RDM_INBOX_SLOTS_MAX;
  _reg_slots = _reg.slots() < RDM_REGISTER_SLOTS_MAX ? _reg.slots() : RDM_REGISTER_SLOTS_MAX;

  uint8_t buf[RECORD_SIZE];
  for (uint16_t i = 0; i < _in_slots; i++) {
    if (!_inbox.read(i, buf)) continue;
    InRecord m;
    unpackIn(buf, m);
    _in_idx[i].recv_time = m.recv_time;
    memcpy(_in_idx[i].sender, m.sender_prefix, 6);
    _in_idx[i].flags = IX_USED;
  }

  // Each row is read once: indexed, and an unsynced one checked against its inbox record right away. A row whose
  // record is gone (a lost file, an interrupted write) goes, so a query says UNKNOWN and the sender resends
  // (D2: duplicate over loss).
  uint8_t linked[(RDM_INBOX_SLOTS_MAX + 7) / 8];
  memset(linked, 0, sizeof(linked));
  for (uint16_t i = 0; i < _reg_slots; i++) {
    if (!_reg.read(i, buf)) continue;
    RegRecord r;
    unpackReg(buf, r);
    indexReg(i, r);
    if (r.state == REG_SYNCED) continue;
    uint16_t s = r.inbox_slot;
    bool ok = s < _in_slots && (_in_idx[s].flags & IX_USED) && !(linked[s / 8] & (1 << (s % 8))) && _inbox.read(s, buf);
    if (ok) {
      InRecord m;
      unpackIn(buf, m);
      ok = samePrefix(r.sender_prefix, m.sender_prefix) && r.ts == m.ts;
    }
    if (ok) {
      linked[s / 8] |= (uint8_t)(1 << (s % 8));
    } else {
      dropRegSlot(i);
    }
  }
  reconcile(linked);
  return true;
}

// An inbox record without a row of its own whose message is already SYNCED is the tail of an interrupted sync.
// One without any row stays: Alice never deletes an unsynced message herself.
void Inbox::reconcile(const uint8_t* linked) {
  uint8_t rbuf[REG_RECORD_SIZE], ibuf[RECORD_SIZE];
  for (uint16_t s = 0; s < _in_slots; s++) {
    if (!(_in_idx[s].flags & IX_USED) || (linked[s / 8] & (1 << (s % 8)))) continue;
    if (!_inbox.read(s, ibuf)) continue;
    InRecord m;
    unpackIn(ibuf, m);
    for (uint16_t i = 0; i < _reg_slots; i++) {
      const RegIdx& x = _reg_idx[i];
      if (!(x.flags & IX_SYNCED) || x.ts != m.ts || !_reg.read(i, rbuf)) continue;
      if (samePrefix(rbuf, m.sender_prefix)) {
        dropInboxSlot(s);
        break;
      }
    }
  }
}

int Inbox::findReg(const uint8_t prefix[6], uint32_t ts, const uint8_t key[4], RegRecord* out) {
  uint8_t buf[REG_RECORD_SIZE];
  for (uint16_t i = 0; i < _reg_slots; i++) {
    const RegIdx& x = _reg_idx[i];
    if (!(x.flags & IX_USED) || x.ts != ts || memcmp(x.key, key, 4) != 0) continue;
    if (!_reg.read(i, buf) || !samePrefix(buf, prefix)) continue;
    if (out) unpackReg(buf, *out);
    return i;
  }
  return -1;
}

int Inbox::findRegForInbox(uint16_t inbox_slot) const {
  for (uint16_t i = 0; i < _reg_slots; i++) {
    const RegIdx& x = _reg_idx[i];
    if ((x.flags & IX_USED) && !(x.flags & IX_SYNCED) && x.inbox_slot == inbox_slot) return i;
  }
  return -1;
}

int Inbox::freeRegSlot() const {
  for (uint16_t i = 0; i < _reg_slots; i++)
    if (!(_reg_idx[i].flags & IX_USED)) return i;
  return -1;
}

int Inbox::freeInboxSlot() const {
  for (uint16_t i = 0; i < _in_slots; i++)
    if (!(_in_idx[i].flags & IX_USED)) return i;
  return -1;
}

uint16_t Inbox::unsyncedFrom(const uint8_t prefix[6]) const {
  uint16_t n = 0;
  for (uint16_t i = 0; i < _in_slots; i++)
    if ((_in_idx[i].flags & IX_USED) && samePrefix(_in_idx[i].sender, prefix)) n++;
  return n;
}

// Frees the synced row with the lowest sender timestamp and raises that sender's watermark first, so a later
// query for it says EVICTED (I7). Rows with a report still owed to the mailbox stay.
// Returns the freed slot, -1 if nothing may be evicted, -2 on a storage error.
int Inbox::evictReg() {
  int victim = -1;
  for (uint16_t i = 0; i < _reg_slots; i++) {
    const RegIdx& x = _reg_idx[i];
    if ((x.flags & (IX_USED | IX_SYNCED | IX_PENDING)) != (IX_USED | IX_SYNCED)) continue;
    if (victim < 0 || x.ts < _reg_idx[victim].ts) victim = i;
  }
  if (victim < 0) return -1;
  uint16_t slot = (uint16_t)victim;

  uint8_t buf[REG_RECORD_SIZE];
  if (_reg.read(slot, buf)) {
    RegRecord r;
    unpackReg(buf, r);
    ContactRdm* c = _contacts.findOrAdd(r.sender_prefix);
    if (!c) return -2;
    if (r.ts > c->watermark) {
      uint32_t old = c->watermark;
      c->watermark = r.ts;
      if (!_contacts.save(*c)) {
        c->watermark = old;
        return -2;
      }
    }
  }
  if (!_reg.erase(slot)) return -2;   // the row stays indexed: the next store() tries again
  _reg_idx[slot].flags = 0;
  return slot;
}

bool Inbox::writeReg(uint16_t slot, const RegRecord& r) {
  uint8_t buf[REG_RECORD_SIZE];
  packReg(r, buf);
  if (!_reg.write(slot, buf)) return false;
  indexReg(slot, r);
  return true;
}

void Inbox::indexReg(uint16_t slot, const RegRecord& r) {
  RegIdx& x = _reg_idx[slot];
  x.ts = r.ts;
  memcpy(x.key, r.key, 4);
  x.inbox_slot = r.inbox_slot;
  x.flags = regIdxFlags(r);
}

// The index forgets the row even when the erase fails: a stale row on flash is repaired at the next boot.
void Inbox::dropRegSlot(uint16_t slot) {
  _reg.erase(slot);
  _reg_idx[slot].flags = 0;
}

// Watermarks describe rows of the lost register; kept, they would answer EVICTED for a message that may
// have been lost with it.
void Inbox::resetWatermarks() {
  for (uint16_t i = 0; i < _contacts.count(); i++) {
    ContactRdm* c = _contacts.at(i);
    if (!c || c->watermark == 0) continue;
    c->watermark = 0;
    _contacts.save(*c);
  }
}

void Inbox::addExtra(const Report& rep) {
  for (uint8_t i = 0; i < _n_extra; i++)
    if (sameReport(_extra[i], rep)) return;
  if (_n_extra < MAX_BATCH) _extra[_n_extra++] = rep;   // full: the mailbox re-sends the copy, we report then
}

void Inbox::dropInboxSlot(uint16_t slot) {
  _inbox.erase(slot);
  _in_idx[slot].flags = 0;
  if (_handed == slot) _handed = NO_SLOT;
}

// Another copy of a known message came from the mailbox. It must learn the outcome, or it keeps the copy
// in SENT/ON_RADIO: SYNCED if the app already has it, otherwise DUPLICATE.
void Inbox::queueMailboxReport(uint16_t slot, const RegRecord& r, const uint8_t mbx_hash[8]) {
  bool same_copy = (r.flags & RF_MBX_OUTCOME) && memcmp(r.mbx_hash, mbx_hash, 8) == 0;
  if ((r.flags & RF_REPORT_PENDING) && same_copy) return;

  RegRecord upd = r;
  memcpy(upd.mbx_hash, mbx_hash, 8);
  upd.flags |= RF_MBX_OUTCOME | RF_REPORT_PENDING;
  if (upd.state == REG_ON_RADIO) upd.flags |= RF_REPORT_DUP;
  else upd.flags &= (uint8_t)~RF_REPORT_DUP;

  // The row still owes a report for another copy, or cannot be written: carry this one in RAM.
  if ((r.flags & RF_REPORT_PENDING) || !writeReg(slot, upd)) addExtra(reportOf(upd));
  _host.onReportQueued();
}

RecvResult Inbox::store(const RecvInput& in, const uint8_t key[4], const RecvDecision& d, uint32_t now) {
  if (in.text_len > INBOX_TEXT_MAX) return RecvResult::STORE_ERROR;
  uint16_t quota = _in_slots / RDM_INBOX_QUOTA_DIV;
  if (unsyncedFrom(in.sender_pub) >= (quota ? quota : 1)) return RecvResult::INBOX_FULL;
  int free_in = freeInboxSlot();
  if (free_in < 0) return RecvResult::INBOX_FULL;
  int free_reg = freeRegSlot();
  if (free_reg < 0) free_reg = evictReg();
  if (free_reg == -1) return RecvResult::INBOX_FULL;
  if (free_reg < 0) return RecvResult::STORE_ERROR;
  uint16_t is = (uint16_t)free_in, rs = (uint16_t)free_reg;

  // Inbox record before register row: a row without its record would answer ON_RADIO for a lost message.
  InRecord m;
  memset(&m, 0, sizeof(m));
  memcpy(m.sender_prefix, in.sender_pub, 6);
  m.ts = in.ts;
  m.recv_time = now;
  m.txt_type = in.txt_type;
  m.path_len = in.path_len;
  m.snr_x4 = in.snr_x4;
  m.flags = (in.via_mailbox ? IF_VIA_MAILBOX : 0) | (in.sender_cap ? IF_SENDER_CAP : 0);
  if (in.via_mailbox) memcpy(m.mbx_hash, in.mbx_hash, 8);
  m.text_len = in.text_len;
  if (in.text_len) memcpy(m.text, in.text, in.text_len);
  uint8_t buf[RECORD_SIZE];
  packIn(m, buf);
  if (!_inbox.write(is, buf)) {
    _inbox.erase(is);
    return RecvResult::STORE_ERROR;
  }

  RegRecord r;
  memset(&r, 0, sizeof(r));
  memcpy(r.sender_prefix, in.sender_pub, 6);
  r.ts = in.ts;
  memcpy(r.key, key, 4);
  memcpy(r.ack_r, d.ack_r, 4);
  memcpy(r.ack_s, d.ack_s, 6);
  r.state = REG_ON_RADIO;
  r.flags = m.flags | (in.via_mailbox ? (RF_MBX_OUTCOME | RF_REPORT_PENDING) : 0);
  memcpy(r.mbx_hash, m.mbx_hash, 8);
  r.inbox_slot = is;
  if (!writeReg(rs, r)) {
    _inbox.erase(is);
    return RecvResult::STORE_ERROR;
  }

  _in_idx[is].recv_time = now;
  memcpy(_in_idx[is].sender, in.sender_pub, 6);
  _in_idx[is].flags = IX_USED;
  return RecvResult::NEW;
}

RecvDecision Inbox::onMessage(const RecvInput& in, uint32_t now) {
  RecvDecision d;
  memset(&d, 0, sizeof(d));
  d.result = RecvResult::STORE_ERROR;
  if (!in.sender_pub || (in.text_len && !in.text) || (in.via_mailbox && !in.mbx_hash)) return d;
  if (_in_slots == 0 || _reg_slots == 0) return d;

  uint8_t key[4];
  crypto::key(key, in.ts, in.text, in.text_len, in.sender_pub);
  crypto::ackR(d.ack_r, in.ts, in.flags, in.text, in.text_len, in.sender_pub);
  crypto::ackS(d.ack_s, in.ts, in.txt_type, in.text, in.text_len, in.sender_pub);

  RegRecord known;
  int ri = findReg(in.sender_pub, in.ts, key, &known);
  if (ri >= 0) {
    d.result = RecvResult::DUPLICATE;
    if (in.via_mailbox) {
      queueMailboxReport((uint16_t)ri, known, in.mbx_hash);
    } else {
      d.send_ack_r = d.cap_byte = true;
      d.send_ack_s = known.state == REG_SYNCED && in.sender_cap;
    }
    return d;
  }

  d.result = store(in, key, d, now);
  if (d.result == RecvResult::NEW) {
    d.send_ack_r = d.cap_byte = !in.via_mailbox;
    _host.onInboxChanged();
    if (in.via_mailbox) _host.onReportQueued();
  } else if (in.via_mailbox) {
    // The mailbox puts the copy back to STORED. No FETCH now: it would only hand us the same copy again.
    Report rep;
    memcpy(rep.pkt_hash, in.mbx_hash, 8);
    rep.result = ReportResult::INBOX_FULL;
    memset(rep.ack, 0, sizeof(rep.ack));
    addExtra(rep);
  }
  return d;
}

bool Inbox::nextForApp(InRecord& out, uint16_t& slot_out) {
  for (;;) {
    int best = -1;
    for (uint16_t i = 0; i < _in_slots; i++) {
      const InIdx& x = _in_idx[i];
      if ((x.flags & (IX_USED | IX_OFFERED)) != IX_USED) continue;
      if (best < 0 || x.recv_time < _in_idx[best].recv_time) best = i;
    }
    if (best < 0) return false;
    uint16_t slot = (uint16_t)best;
    uint8_t buf[RECORD_SIZE];
    if (_inbox.read(slot, buf)) {
      unpackIn(buf, out);
      slot_out = slot;
      return true;
    }
    // Unreadable record: drop its row too, so the slot's next owner is not confused with it and a query
    // lets the sender resend.
    int ri = findRegForInbox(slot);
    if (ri >= 0) dropRegSlot((uint16_t)ri);
    dropInboxSlot(slot);
  }
}

void Inbox::onHandedToApp(uint16_t slot) {
  if (slot >= _in_slots || !(_in_idx[slot].flags & IX_USED)) return;
  _in_idx[slot].flags |= IX_OFFERED;
  _handed = slot;
}

void Inbox::onNextSyncRequest(uint32_t now) {
  (void)now;
  if (_handed == NO_SLOT) return;
  uint16_t slot = _handed;
  _handed = NO_SLOT;
  if (!(_in_idx[slot].flags & IX_USED)) return;

  bool ack_s = false, report = false;
  RegRecord r;
  int ri = findRegForInbox(slot);
  uint8_t buf[REG_RECORD_SIZE];
  if (ri >= 0 && _reg.read((uint16_t)ri, buf)) {
    unpackReg(buf, r);
    r.state = REG_SYNCED;
    r.inbox_slot = NO_SLOT;
    r.flags &= (uint8_t)~RF_REPORT_DUP;
    if (r.flags & RF_MBX_OUTCOME) r.flags |= RF_REPORT_PENDING;
    // Not persisted: the message stays unsynced and is offered again on the next connection (D2).
    if (!writeReg((uint16_t)ri, r)) return;
    ack_s = !(r.flags & IF_VIA_MAILBOX) && (r.flags & IF_SENDER_CAP);
    report = r.flags & RF_MBX_OUTCOME;
  } else if (ri >= 0) {
    dropRegSlot((uint16_t)ri);   // unreadable row: must not stay linked to a slot that is about to be reused
  }
  dropInboxSlot(slot);   // a failed erase is finished by reconcile() at boot
  if (ack_s) _host.sendAckS(r.sender_prefix, r.ack_s);
  if (report) _host.onReportQueued();
}

void Inbox::onClientDisconnected() {
  for (uint16_t i = 0; i < _in_slots; i++) _in_idx[i].flags &= (uint8_t)~IX_OFFERED;
  _handed = NO_SLOT;
}

QueryReply Inbox::query(const uint8_t sender_prefix[6], const QueryItem& q) {
  QueryReply rep;
  memset(&rep, 0, sizeof(rep));
  rep.state = QueryState::UNKNOWN;
  RegRecord r;
  if (findReg(sender_prefix, q.ts, q.key, &r) >= 0) {
    if (r.state == REG_SYNCED) {
      rep.state = QueryState::SYNCED;
      memcpy(rep.ack, r.ack_s, 6);
    } else {
      rep.state = QueryState::ON_RADIO;
      memcpy(rep.ack, r.ack_r, 4);
    }
    return rep;
  }
  const ContactRdm* c = _contacts.find(sender_prefix);
  if (c && c->watermark && q.ts <= c->watermark) rep.state = QueryState::EVICTED;
  return rep;
}

uint8_t Inbox::pendingReports(Report* out, uint8_t max) {
  uint8_t n = 0;
  for (uint8_t i = 0; i < _n_extra && n < max; i++) out[n++] = _extra[i];
  uint8_t buf[REG_RECORD_SIZE];
  for (uint16_t i = 0; i < _reg_slots && n < max; i++) {
    if ((_reg_idx[i].flags & (IX_USED | IX_PENDING)) != (IX_USED | IX_PENDING) || !_reg.read(i, buf)) continue;
    RegRecord r;
    unpackReg(buf, r);
    if (r.flags & RF_MBX_OUTCOME) out[n++] = reportOf(r);
  }
  return n;
}

// A confirmation clears a row only if its current report is the one confirmed: a SYNCED report that replaced
// an in-flight ON_RADIO report stays pending.
void Inbox::onReportsConfirmed(const Report* sent, uint8_t n_ok) {
  uint8_t buf[REG_RECORD_SIZE];
  for (uint8_t k = 0; k < n_ok; k++) {
    bool done = false;
    for (uint8_t e = 0; e < _n_extra && !done; e++) {
      if (!sameReport(_extra[e], sent[k])) continue;
      memmove(&_extra[e], &_extra[e + 1], (_n_extra - e - 1) * sizeof(Report));
      _n_extra--;
      done = true;
    }
    for (uint16_t i = 0; i < _reg_slots && !done; i++) {
      if ((_reg_idx[i].flags & (IX_USED | IX_PENDING)) != (IX_USED | IX_PENDING) || !_reg.read(i, buf)) continue;
      RegRecord r;
      unpackReg(buf, r);
      if (!(r.flags & RF_MBX_OUTCOME) || !sameReport(reportOf(r), sent[k])) continue;
      r.flags &= (uint8_t)~(RF_REPORT_PENDING | RF_REPORT_DUP);
      writeReg(i, r);   // on failure the report is sent again; the mailbox treats repeats as no-ops
      done = true;
    }
  }
}

// Alice cannot decrypt the copy (she does not know the sender, L10): the mailbox marks it REJECTED.
void Inbox::onUndecryptable(const uint8_t mbx_hash[8]) {
  Report rep;
  memcpy(rep.pkt_hash, mbx_hash, 8);
  rep.result = ReportResult::UNDECRYPTABLE;
  memset(rep.ack, 0, sizeof(rep.ack));
  addExtra(rep);
  _host.onReportQueued();
}

uint16_t Inbox::freeSlots() const {
  uint16_t n = 0;
  for (uint16_t i = 0; i < _in_slots; i++)
    if (!(_in_idx[i].flags & IX_USED)) n++;
  return n;
}

}
