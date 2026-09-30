#include "RdmFetcher.h"

#include "RdmConfig.h"
#include "RdmPolicy.h"

#include <string.h>

namespace rdm {

namespace {

constexpr uint32_t NONE = UINT32_MAX;

}  // namespace

Fetcher::Fetcher(Inbox& inbox, FetcherHost& host)
    : _inbox(inbox), _host(host), _store_id(0), _next_due(NONE), _trigger_at(NONE), _last_sent(0), _timeout_at(0),
      _retry_at(NONE), _sent_once(false), _in_flight(false), _last_was_retry(false), _sent_n(0) {}

void Fetcher::begin(uint32_t now, uint32_t store_id, uint32_t random32) {
  _store_id = store_id;
  _next_due = now + random32 % (RDM_FETCH_BOOT_JITTER_S + 1);
  _trigger_at = NONE;
  _retry_at = NONE;
  _in_flight = false;
  _sent_once = false;
  _last_was_retry = false;
  _sent_n = 0;
}

void Fetcher::trigger(uint32_t now) {
  uint32_t t = now;
  if (_sent_once && _last_sent + RDM_MBX_REQ_GAP_S > t) t = _last_sent + RDM_MBX_REQ_GAP_S;
  if (t < _trigger_at) _trigger_at = t;
}

bool Fetcher::send(uint32_t now, bool is_retry) {
  if (!_host.canFetch()) return false;   // saves reading the register for a FETCH that cannot go out
  Report r[MAX_BATCH];
  uint8_t n = _inbox.pendingReports(r, MAX_BATCH);
  uint8_t flags = _inbox.freeSlots() == 0 ? FETCH_FLAG_NO_PAYLOAD : 0;
  uint32_t est_ms = 0;
  if (!_host.sendFetch(flags, _store_id, r, n, est_ms)) return false;

  memcpy(_sent, r, n * sizeof(Report));
  _sent_n = n;
  _in_flight = true;
  _sent_once = true;
  _last_sent = now;
  _trigger_at = NONE;
  _retry_at = NONE;
  _last_was_retry = is_retry;
  // G17a/G17b: the interval gets its jitter, a repeat keeps the original's interval
  if (!is_retry) _next_due = now + jittered(RDM_FETCH_INTERVAL_S, _host.random32());
  _timeout_at = now + waitSecs(est_ms);
  return true;
}

void Fetcher::onFetchReply(uint8_t remaining, uint8_t reports_ok, bool had_payload, uint32_t now) {
  (void)had_payload;
  // A reply that arrives after its time-out still counts: the mailbox did process those reports.
  uint8_t n_ok = reports_ok < _sent_n ? reports_ok : _sent_n;
  if (n_ok) _inbox.onReportsConfirmed(_sent, n_ok);
  bool all_taken = n_ok == _sent_n;
  _in_flight = false;
  _retry_at = NONE;   // the mailbox answers after all

  Report pending[MAX_BATCH];
  uint8_t n = _inbox.pendingReports(pending, MAX_BATCH);
  bool inbox_full_report = false, unsent_report = false;
  for (uint8_t i = 0; i < n; i++) {
    if (pending[i].result == ReportResult::INBOX_FULL) {
      inbox_full_report = true;
      continue;
    }
    bool was_sent = false;
    for (uint8_t k = 0; k < _sent_n && !was_sent; k++) was_sent = sameReport(pending[i], _sent[k]);
    unsent_report |= !was_sent;
  }
  _sent_n = 0;

  // A copy we just had to refuse would come straight back; wait for the interval (or a sync) instead.
  if (remaining > 0 && _inbox.freeSlots() > 0 && !inbox_full_report) trigger(now);
  // Reports the mailbox refused are not pushed again right away; new ones (overflow of a full batch) are.
  if (all_taken && unsent_report) trigger(now);
}

void Fetcher::onFetchTimeout(uint32_t now) {
  if (!_in_flight) return;
  _in_flight = false;
  _trigger_at = NONE;
  // G17b: one repeat after 60-120 s (a collision is the likely cause), then only the interval, so an
  // unreachable mailbox is not polled every time-out
  if (!_last_was_retry) _retry_at = now + firstRetryDelay(_host.random32());
}

void Fetcher::onMailboxAdvert(uint32_t now) {
  if (_next_due == NONE || _in_flight) return;
  if (!_sent_once || now - _last_sent >= RDM_MBX_ADVERT_MIN_S) trigger(now);
}

void Fetcher::onReportQueued(uint32_t now) {
  if (_next_due == NONE) return;
  trigger(now);
}

void Fetcher::loop(uint32_t now) {
  if (_next_due == NONE) return;
  if (_in_flight) {
    if (now >= _timeout_at) onFetchTimeout(now);
    return;
  }
  uint32_t due = _trigger_at < _next_due ? _trigger_at : _next_due;
  bool is_retry = now < due && now >= _retry_at;
  if (now < due && !is_retry) return;
  uint8_t mbx[32];
  if (!_host.ownMailbox(mbx)) return;
  send(now, is_retry);   // on failure the FETCH stays due and the next loop tries again
}

uint32_t Fetcher::nextDue(uint32_t now) const {
  uint8_t mbx[32];
  if (_next_due == NONE || !_host.ownMailbox(mbx)) return NONE;
  uint32_t due = _trigger_at < _next_due ? _trigger_at : _next_due;
  if (_retry_at < due) due = _retry_at;
  if (_in_flight) due = _timeout_at;
  return due < now ? now : due;
}

}
