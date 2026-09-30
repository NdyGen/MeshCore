#pragma once

#include "RdmInbox.h"

namespace rdm {

class FetcherHost {
public:
  virtual ~FetcherHost() {}
  virtual bool ownMailbox(uint8_t mbx_pub_out[32]) = 0;           // false: no mailbox configured
  virtual bool sendFetch(uint8_t flags, uint32_t store_id, const Report* r, uint8_t n, uint32_t& est_timeout_ms) = 0;
  virtual uint32_t random32() = 0;                                  // jitter (G17)
  virtual bool canFetch() { return true; }                          // false: sendFetch would refuse right now
};

class Fetcher {
public:
  Fetcher(Inbox& inbox, FetcherHost& host);
  void begin(uint32_t now, uint32_t store_id, uint32_t random32);   // boot FETCH after 0-60 s jitter
  void onFetchReply(uint8_t remaining, uint8_t reports_ok, bool had_payload, uint32_t now);
  void onFetchTimeout(uint32_t now);
  void onMailboxAdvert(uint32_t now);
  void onReportQueued(uint32_t now);
  void loop(uint32_t now);
  // Earliest RDM time (s) of the next FETCH, boot FETCH or fetch timeout. UINT32_MAX: no own mailbox.
  uint32_t nextDue(uint32_t now) const;

private:
  Inbox&       _inbox;
  FetcherHost& _host;

  uint32_t _store_id;
  uint32_t _next_due;               // boot FETCH, then the 60-min interval; UINT32_MAX before begin()
  uint32_t _trigger_at;             // earliest extra FETCH (sync, advert, continuation); UINT32_MAX: none
  uint32_t _last_sent;
  uint32_t _timeout_at;
  uint32_t _retry_at;               // G17b: the one repeat after an unanswered FETCH; UINT32_MAX: none
  bool     _sent_once;
  bool     _in_flight;
  bool     _last_was_retry;
  uint8_t  _sent_n;
  Report   _sent[MAX_BATCH];

  void trigger(uint32_t now);
  bool send(uint32_t now, bool is_retry);
};

}
