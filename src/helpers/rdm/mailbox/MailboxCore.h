#pragma once

#include <helpers/rdm/RdmConfig.h>
#include <helpers/rdm/RdmTypes.h>

namespace rdm {

struct BackendReply {
  enum class Kind : uint8_t { STORE, REG, FETCH, STAT } kind;
  uint32_t id;
  MbxCode  code;
  uint32_t expires;                                  // STORE: absolute Pi time; MailboxCore sends expires - backendTime() as ttl_s
  uint8_t  ttl_days, quota;                          // REG
  uint8_t  remaining, reports_ok, inner_len;         // FETCH
  uint8_t  inner[MAX_INNER_PAYLOAD];
  uint8_t  n; StatusReply items[MAX_BATCH];          // STAT
};

class MailboxBackend {
public:
  virtual ~MailboxBackend() {}
  virtual bool store(uint32_t id, const uint8_t owner[4], const uint8_t sender_pub[32], const uint8_t pkt_hash[8],
                     const uint8_t* payload, uint8_t len) = 0;
  virtual bool reg(uint32_t id, const uint8_t sender_pub[32], const uint8_t owner[4], const uint8_t token[8]) = 0;
  virtual bool fetch(uint32_t id, const uint8_t client_pub[32], uint8_t flags, uint32_t store_id, const Report* r, uint8_t n) = 0;
  virtual bool stat(uint32_t id, const uint8_t sender_pub[32], const uint8_t (*hashes)[8], uint8_t n) = 0;
  virtual bool poll(BackendReply& out) = 0;                                    // non-blocking
  virtual bool pollAcl(uint8_t pub_out[32], bool& is_owner, uint8_t owner_out[4]) = 0;   // fill cache after boot
  virtual bool ready() = 0;                                                    // mbx.ready received
  virtual uint32_t backendTime() = 0;                                          // Pi unix time now (last mbx.time + elapsed); 0 = unknown
};

class MailboxCoreHost {
public:
  virtual ~MailboxCoreHost() {}
  virtual uint32_t millis() = 0;
  virtual bool txIdle() = 0;
  virtual bool addPeer(const uint8_t pub[32]) = 0;            // peer cache for decryption, LRU 64
  virtual bool sendResponse(const uint8_t client_pub[32], uint32_t tag, const uint8_t* body, size_t len, bool path_return) = 0;
};

class MailboxCore {
public:
  MailboxCore(MailboxBackend& be, MailboxCoreHost& host);
  void begin();
  void onRequest(const uint8_t client_pub[32], const uint8_t* data, size_t len, bool via_flood);   // data = ts(4) | body
  void onAnonRequest(const uint8_t client_pub[32], const uint8_t* plain, size_t len, bool via_flood);   // registration
  void loop();                                                  // backend replies, 3 s timeout = NO_STORAGE, limits
  // Milliseconds until loop() has timed work (backend timeout, paced response). 0: due now, UINT32_MAX: none.
  // Backend replies are events: the host polls after serial input or backend activity.
  uint32_t nextWakeupMillis(uint32_t millis_now) const;

private:
  MailboxBackend&  _be;
  MailboxCoreHost& _host;

  static const uint8_t  CLIENTS = RDM_MBX_CLIENTS;
  static_assert(RDM_MBX_CLIENTS <= 255, "client count is 8 bit");
  static const uint8_t  PENDING = 8;            // backend requests in flight
  static const uint8_t  OUTQ = 4;               // responses waiting for an idle transmitter
  static const uint8_t  OWNER_TTLS = 8;
  static const uint8_t  RESP_MAX = 3 + MAX_INNER_PAYLOAD;

  struct Client {
    uint8_t  pub[32];
    bool     used, has_ts, has_req;
    uint32_t last_ts, last_req_ms, hour_start_ms, lru;
    uint8_t  hour_count;
  };
  // Where a response goes: the client, its request timestamp as tag, and a PATH return for a flood request
  struct ReplyTo {
    uint8_t  client[32];
    uint32_t tag;
    bool     path_return;
  };
  struct Pending {
    bool     used;
    BackendReply::Kind kind;
    uint32_t id, deadline_ms;
    ReplyTo  to;
    uint8_t  owner[4];
    uint8_t  pkt_hash[8];
  };
  struct Outgoing {
    ReplyTo  to;
    uint8_t  len;
    uint8_t  body[RESP_MAX];
  };
  struct OwnerTtl { bool used; uint8_t owner[4]; uint8_t ttl_days; };

  Client   _clients[CLIENTS];
  Pending  _pending[PENDING];
  Outgoing _out[OUTQ];
  OwnerTtl _owner_ttl[OWNER_TTLS];
  uint8_t  _out_head = 0, _out_count = 0;
  uint32_t _next_id = 1, _lru_seq = 0;
  uint32_t _anon_window_ms = 0;
  uint8_t  _anon_count = 0;
  bool     _anon_window_set = false;

  Client*  client(const uint8_t pub[32]);
  bool     admit(Client& c, uint32_t ts, uint32_t now);
  bool     rateLimited(const Client& c, uint32_t now) const;
  Pending* newPending(BackendReply::Kind kind, const ReplyTo& to, uint32_t now);
  void     respond(const ReplyTo& to, const uint8_t* body, size_t len);
  void     answerStore(const ReplyTo& to, const uint8_t pkt_hash[8], MbxCode code, uint32_t ttl_s);
  void     answerReg(const ReplyTo& to, MbxCode code, uint8_t ttl_days, uint8_t quota);
  void     onDeposit(const ReplyTo& to, const uint8_t owner[4], const uint8_t* inner, uint8_t inner_len, bool limited,
                     uint32_t now);
  void     onBackendReply(const BackendReply& r);
  void     rememberOwnerTtl(const uint8_t owner[4], uint8_t ttl_days);
  uint32_t fallbackTtl(const uint8_t owner[4]) const;
};

}
