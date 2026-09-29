#pragma once

#include "RdmStorage.h"

// /rdm/meta is a RecordFile with one A/B slot: store_id(4) | rdm_time(4) | last_req_ts(4).

namespace rdm {

class Clock {
public:
  static constexpr uint16_t    RECORD_SIZE = 12;
  static constexpr const char* PATH = "/rdm/meta";

  explicit Clock(RecordFile& meta);
  bool     begin(uint32_t millis_now, bool storage_recreated, uint32_t random32);  // new store_id on recreate
  uint32_t now(uint32_t millis_now, uint32_t rtc_now, bool rtc_trusted);          // G6, monotonic
  void     maybePersist(uint32_t millis_now, bool force);                          // every RDM_CLOCK_PERSIST_S or force
  uint32_t nextReqTimestamp(uint32_t rtc_now);                                     // max(rtc_now, last + 1), persistent (K2)
  uint32_t storeId() const;
  uint32_t nextPersistMillis() const;                                              // millis at which maybePersist() writes
  // Milliseconds from millis_now until RDM time reaches rdm_due, assuming it advances with millis. May be early
  // (a trusted RTC can jump ahead), never late. 0: already due.
  uint32_t millisUntil(uint32_t rdm_due, uint32_t millis_now) const;

private:
  RecordFile& _meta;
  uint32_t    _store_id = 0;
  uint32_t    _base_time = 0;       // RDM time at millis _base_ms
  uint32_t    _base_ms = 0;
  uint32_t    _last_req = 0;
  uint32_t    _last_persist_ms = 0;

  void fold(uint32_t millis_now);
  bool persist();
};

}
