#include "RdmContacts.h"

#include "RdmBytes.h"

#include <string.h>

namespace rdm {

static void encode(const ContactRdm& c, uint8_t* r) {
  memcpy(r, c.pub_prefix, 6);
  r[6] = c.flags;
  r[7] = c.reserved;
  put32(&r[8], c.watermark);
  memcpy(&r[12], c.mbx_pub, 32);
  memcpy(&r[44], c.token, 8);
}

static void decode(const uint8_t* r, ContactRdm& c) {
  memcpy(c.pub_prefix, r, 6);
  c.flags = r[6];
  c.reserved = r[7];
  c.watermark = get32(&r[8]);
  memcpy(c.mbx_pub, &r[12], 32);
  memcpy(c.token, &r[44], 8);
}

// Out-of-class definitions for C++11 builds (nRF52): Node binds these members to references.
constexpr uint16_t    ContactTable::RECORD_SIZE;
constexpr const char* ContactTable::PATH;

ContactTable::ContactTable(RecordFile& file) : _file(file) {
}

bool ContactTable::begin() {
  _n = 0;
  _use_clock = 0;
  if (_file.payloadSize() != RECORD_SIZE || _file.open() == RecordFile::Open::FAILED) return false;
  _n = _file.slots() < RDM_CONTACT_SLOTS_MAX ? _file.slots() : RDM_CONTACT_SLOTS_MAX;
  uint8_t rec[RECORD_SIZE];
  for (uint16_t i = 0; i < _n; i++) {
    _last_use[i] = 0;
    if (_file.read(i, rec)) {
      decode(rec, _c[i]);
      _last_use[i] = ++_use_clock;   // after a reboot the slot order stands in for recency
    }
  }
  return true;
}

int ContactTable::indexOf(const uint8_t pub_prefix[6]) const {
  for (uint16_t i = 0; i < _n; i++) {
    if (_last_use[i] && memcmp(_c[i].pub_prefix, pub_prefix, 6) == 0) return i;
  }
  return -1;
}

// Only entries without a mailbox and without a watermark are evictable: losing CR_CAP or CR_MBX_INFO_SENT costs
// at most one extra DM or MBX_INFO, while a lost watermark or mailbox changes delivery.
int ContactTable::slotFor(const uint8_t pub_prefix[6]) const {
  int idx = indexOf(pub_prefix);
  if (idx >= 0) return idx;
  int victim = -1;
  for (uint16_t i = 0; i < _n; i++) {
    if (_last_use[i] == 0) return i;
    if ((_c[i].flags & CR_HAS_MBX) || _c[i].watermark != 0) continue;
    if (victim < 0 || _last_use[i] < _last_use[victim]) victim = i;
  }
  return victim;
}

bool ContactTable::store(int idx, const ContactRdm& c) {
  uint8_t rec[RECORD_SIZE];
  encode(c, rec);
  if (!_file.write((uint16_t)idx, rec)) return false;
  if (&_c[idx] != &c) _c[idx] = c;
  _last_use[idx] = ++_use_clock;
  return true;
}

ContactRdm* ContactTable::find(const uint8_t pub_prefix[6]) {
  int idx = indexOf(pub_prefix);
  if (idx < 0) return nullptr;
  _last_use[idx] = ++_use_clock;
  return &_c[idx];
}

ContactRdm* ContactTable::findOrAdd(const uint8_t pub_prefix[6]) {
  ContactRdm* found = find(pub_prefix);
  if (found) return found;
  int idx = slotFor(pub_prefix);
  if (idx < 0) return nullptr;
  ContactRdm fresh;
  memset(&fresh, 0, sizeof(fresh));
  memcpy(fresh.pub_prefix, pub_prefix, 6);
  return store(idx, fresh) ? &_c[idx] : nullptr;
}

bool ContactTable::save(const ContactRdm& c) {
  int idx = slotFor(c.pub_prefix);
  return idx >= 0 && store(idx, c);
}

ContactRdm* ContactTable::findByMailbox(const uint8_t mbx_pub_prefix[6]) {
  for (uint16_t i = 0; i < _n; i++) {
    if (_last_use[i] && (_c[i].flags & CR_HAS_MBX) && memcmp(_c[i].mbx_pub, mbx_pub_prefix, 6) == 0) return &_c[i];
  }
  return nullptr;
}

uint16_t ContactTable::count() const {
  return _n;
}

ContactRdm* ContactTable::at(uint16_t i) {
  return i < _n && _last_use[i] ? &_c[i] : nullptr;
}

}
