#pragma once

// Times in seconds of RDM time (03 par. 11, G6). Slot maxima are overridable for tests and small boards.

#ifndef RDM_OUTBOX_SLOTS_MAX
  #define RDM_OUTBOX_SLOTS_MAX    16
#endif
#ifndef RDM_REGISTER_SLOTS_MAX
  #define RDM_REGISTER_SLOTS_MAX  512
#endif
#ifndef RDM_INBOX_SLOTS_MAX
  #define RDM_INBOX_SLOTS_MAX     256
#endif
#ifndef RDM_WATCH_SLOTS_MAX
  #define RDM_WATCH_SLOTS_MAX     128
#endif
#ifndef RDM_CONTACT_SLOTS_MAX
  #define RDM_CONTACT_SLOTS_MAX   64
#endif
#ifndef RDM_MIN_FREE_BYTES
  #define RDM_MIN_FREE_BYTES      16384    // less free space: RDM off (03 par. 6)
#endif
#define RDM_HIDDEN_PEERS          4
#define RDM_T_RADIO_S             (7UL * 86400)
#define RDM_T_SYNC_S              (30UL * 86400)
#define RDM_WATCH_S               (90UL * 86400)
#define RDM_FINAL_KEEP_S          86400        // G2, default D3
#define RDM_CUSTODY_GRACE_S       86400        // G10
#define RDM_T_MAX_CUSTODY_S       (30UL * 86400)   // G10: M's ttl_s, at most 30 days
#define RDM_RETRY_AFTER_LOSS_S    86400        // decision WP3: at least a day more after M or Alice lost the copy
#define RDM_WAIT_ACK_MIN_S        30           // G3: max(2 x est_timeout, 30 s)
#define RDM_OUTBOX_TX_GAP_S       60
#define RDM_BOOT_KICK_MIN_S       60           // G6
#define RDM_BOOT_KICK_MAX_S       120
#define RDM_CLOCK_PERSIST_S       600
#define RDM_HEARD_MIN_S           3600         // hear trigger in RETRY/ON_RADIO/CUSTODY
#define RDM_MBX_ADVERT_MIN_S      600
#define RDM_FETCH_INTERVAL_S      3600
#define RDM_FETCH_BOOT_JITTER_S   60
#define RDM_MBX_REQ_GAP_S         6            // min gap between REQs to one mailbox: the server's gap + 1 s
#define RDM_MBX_SERVER_REQ_GAP_S  5            // MailboxCore accepts 1 REQ per 5 s per client (03 par. 5)
#ifndef RDM_MBX_CLIENTS
  #define RDM_MBX_CLIENTS         64           // clients a mailbox tracks: its peer cache and MailboxCore's rate limits
#endif
#define RDM_INBOX_QUOTA_DIV       4            // max 1/4 of the inbox per sender (K8)
// schedules (03 par. 5), seconds after the previous point; the last value repeats
#define RDM_SCHED_DM_BACKOFF      { 300, 900, 3600, 14400, 43200, 86400 }
#define RDM_SCHED_PROBE_DAY1      1800         // first 24 h
#define RDM_SCHED_PROBE_LATER     7200
#define RDM_PROBE_FLOOD_MIN_S     14400
#define RDM_DM_EVEN_WITH_CAP_S    86400        // G15
#define RDM_SCHED_ON_RADIO        { 3600, 14400, 43200, 86400 }
#define RDM_SCHED_STATUS          { 600, 1800, 3600, 14400, 43200 }
#define RDM_SCHED_REDEPOSIT       { 600, 1800, 3600, 14400, 43200 }   // G13
#define RDM_CUSTODY_PROBE_S       86400
#define RDM_RETRY_FIRST_MIN_S     60           // G16: first RETRY action 60-120 s after the timeout
#define RDM_RETRY_FIRST_MAX_S     120
#define RDM_JITTER_MAX_S          60           // G17: every repeating interval I becomes I + U[0, min(I / RDM_JITTER_DIV, RDM_JITTER_MAX_S)]
#define RDM_JITTER_DIV            10

static_assert(RDM_MBX_REQ_GAP_S > RDM_MBX_SERVER_REQ_GAP_S, "a client REQ must never hit the mailbox's rate limit");
