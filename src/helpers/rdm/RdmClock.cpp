#include "RdmClock.h"

#include "RdmBytes.h"
#include "RdmConfig.h"

namespace rdm {

// Longest wait millisUntil() reports: keeps millis_now + wait clear of wrap-around in signed comparisons.
static const uint32_t MAX_WAIT_MS = 0x7FFFFFFF;

// Out-of-class definitions for C++11 builds (nRF52): Node binds these members to references.
constexpr uint16_t    Clock::RECORD_SIZE;
constexpr const char* Clock::PATH;

Clock::Clock(RecordFile& meta) : _meta(meta) {
}

bool Clock::begin(uint32_t millis_now, bool storage_recreated, uint32_t random32) {
  if (_meta.payloadSize() != RECORD_SIZE || _meta.open() == RecordFile::Open::FAILED) return false;
  uint8_t rec[RECORD_SIZE];
  bool have = _meta.read(0, rec);
  if (have) {
    _store_id = get32(rec);
    _base_time = get32(&rec[4]);
    _last_req = get32(&rec[8]);
  }
  _base_ms = millis_now;
  _last_persist_ms = millis_now;
  // Without the meta record the old store_id is unknown, so the mailbox must see a new store as well (G12).
  if (have && !storage_recreated) return true;
  _store_id = random32 ? random32 : 1;
  return persist();
}

void Clock::fold(uint32_t millis_now) {
  uint32_t secs = (millis_now - _base_ms) / 1000;
  _base_time += secs;
  _base_ms += secs * 1000;
}

uint32_t Clock::now(uint32_t millis_now, uint32_t rtc_now, bool rtc_trusted) {
  fold(millis_now);
  if (rtc_trusted && rtc_now > _base_time) {
    _base_time = rtc_now;
    _base_ms = millis_now;
  }
  return _base_time;
}

bool Clock::persist() {
  uint8_t rec[RECORD_SIZE];
  put32(rec, _store_id);
  put32(&rec[4], _base_time);
  put32(&rec[8], _last_req);
  return _meta.write(0, rec);
}

void Clock::maybePersist(uint32_t millis_now, bool force) {
  if (!force && millis_now - _last_persist_ms < RDM_CLOCK_PERSIST_S * 1000) return;
  fold(millis_now);
  persist();
  _last_persist_ms = millis_now;
}

uint32_t Clock::nextReqTimestamp(uint32_t rtc_now) {
  _last_req = rtc_now > _last_req ? rtc_now : _last_req + 1;
  persist();
  return _last_req;
}

uint32_t Clock::storeId() const {
  return _store_id;
}

uint32_t Clock::nextPersistMillis() const {
  return _last_persist_ms + RDM_CLOCK_PERSIST_S * 1000;
}

// UINT32_MAX (nothing due) passes through unchanged.
uint32_t Clock::millisUntil(uint32_t rdm_due, uint32_t millis_now) const {
  if (rdm_due == UINT32_MAX) return UINT32_MAX;
  if (rdm_due <= _base_time) return 0;
  uint64_t due_ms = (uint64_t)(rdm_due - _base_time) * 1000;
  uint32_t elapsed = millis_now - _base_ms;
  if (due_ms <= elapsed) return 0;
  uint64_t wait = due_ms - elapsed;
  return wait > MAX_WAIT_MS ? MAX_WAIT_MS : (uint32_t)wait;
}

}
