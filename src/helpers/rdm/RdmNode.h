#pragma once

#include "RdmFetcher.h"
#include "RdmInbox.h"
#include "RdmOutbox.h"
#include "RdmStorage.h"
#include "RdmTypes.h"
#include "RdmClock.h"
#include "RdmContacts.h"

#include <new>

namespace rdm {

class NodeHost {
public:
  virtual ~NodeHost() {}
  // time and randomness
  virtual uint32_t millis() = 0;
  virtual uint32_t rtcNow(bool& trusted) = 0;
  virtual uint32_t random32() = 0;
  virtual bool     txIdle() = 0;
  virtual void     selfPub(uint8_t pub_out[32]) = 0;       // own identity (self_id.pub_key)
  // contacts
  virtual bool lookupContact(const uint8_t pub_prefix[6], uint8_t pub_out[32], bool& favourite, bool& has_path) = 0;
  // sending (implemented by RdmChatMesh)
  virtual bool sendTxtPlain(const uint8_t pub_prefix[6], const uint8_t* plain, size_t len, bool flood, uint32_t& est_timeout_ms) = 0;
  virtual bool sendAck(const uint8_t pub_prefix[6], const uint8_t* ack, uint8_t len) = 0;
  virtual bool sendReq(const uint8_t peer_pub[32], uint32_t req_ts, const uint8_t* body, size_t len, bool flood,
                       uint32_t& est_timeout_ms) = 0;
  virtual bool sendAnonReq(const uint8_t peer_pub[32], const uint8_t* plain, size_t len, uint32_t& est_timeout_ms) = 0;
  virtual bool encryptTxtPayload(const uint8_t pub_prefix[6], const uint8_t* plain, size_t len,
                                 uint8_t* payload_out, size_t& payload_len) = 0;
  virtual bool decryptTxtPayload(const uint8_t* payload, size_t len, uint8_t sender_pub_out[32],
                                 uint8_t* plain_out, size_t& plain_len) = 0;
  // app side (companion or simulator)
  virtual bool pushUserStatus(const uint8_t pub_prefix[6], uint32_t app_ack, UserStatus s, const uint8_t key[4], uint32_t ts) = 0;
  virtual bool pushSendConfirmed(uint32_t app_ack) = 0;
  virtual void pushMsgWaiting() = 0;
  // K3: the set of hidden peers (own mailbox, contacts' mailboxes) changed, also once after begin()
  virtual void onHiddenPeersChanged() {}
};

class Node : private OutboxHost, private InboxHost, private FetcherHost {
public:
  static constexpr uint16_t    MBX_RECORD_SIZE = 32 + 16;   // own mailbox: pubkey | K_owner
  static constexpr const char* MBX_PATH = "/rdm/mbx";

  Node(FileIO& io, NodeHost& host);
  bool begin();                      // open files, slot counts from io.freeBytes(); false = RDM off
  bool enabled() const;
  void setEnabled(bool on);          // runtime off: exact upstream behaviour (scenario 10, stock Alice in the simulator)
  void loop();
  // Milliseconds from millis_now until loop() has scheduled work: min over Outbox/Fetcher::nextDue (via
  // Clock::millisUntil) and Clock::nextPersistMillis. 0: due now, UINT32_MAX: nothing scheduled. May be early,
  // never late. For the simulator's fast-forward and for sleep decisions on the device.
  uint32_t nextWakeupMillis(uint32_t millis_now) const;

  // sender
  struct AppSend { bool handled; bool transmit; bool cap_trailer; uint8_t attempt; uint32_t app_ack; };
  AppSend onAppSend(const uint8_t pub_prefix[6], uint32_t ts, uint8_t attempt, const char* text, size_t text_len);
  void    onAppTransmitted(const uint8_t pub_prefix[6], uint32_t ts, uint32_t est_timeout_ms);

  // receiving (called by RdmChatMesh)
  RecvDecision onPlainTxt(const uint8_t sender_pub[32], const uint8_t* data, size_t len, uint8_t path_len, int8_t snr_x4);
  void    onCtrlTxt(const uint8_t sender_pub[32], const uint8_t* data, size_t len);
  bool    onAck(const uint8_t* ack, uint8_t len);
  uint8_t onReceiptQuery(const uint8_t sender_pub[32], const uint8_t* body, size_t len, uint8_t* reply_body);
  bool    onResponse(const uint8_t peer_pub[32], const uint8_t* data, size_t len);   // true: tag belonged to RDM
  void    onHeard(const uint8_t pub_prefix[6], bool had_cap_trailer);
  void    onAdvert(const uint8_t pub[32]);

  // hidden peers (mailboxes) for RdmChatMesh::searchPeersByHash
  uint8_t        hiddenPeerCount() const;
  const uint8_t* hiddenPeerPub(uint8_t i) const;

  // app side
  bool    nextInboxFrame(InRecord& out, uint16_t& slot);
  void    onInboxFrameHanded(uint16_t slot);
  void    onSyncRequest();
  void    onClientConnected(bool rdm_client);
  void    onClientDisconnected();
  bool    setOwnMailbox(const uint8_t* mbx_pub, const uint8_t* k_owner);   // nullptr, nullptr = none
  uint8_t listOutbox(OutEntry* out, uint8_t max, uint8_t offset = 0);   // entries offset.. (streaming, no big buffer)

private:
  FileIO&   _io;
  NodeHost& _host;

  // Record files and modules are built in begin(), when the flash is mounted and slot counts can be derived
  // from its free space; in-place storage avoids the heap.
  template <class T> class Late {
    alignas(T) uint8_t _buf[sizeof(T)];
    bool _live = false;
  public:
    ~Late() { reset(); }
    template <class... A> T& make(A&&... a) { reset(); _live = true; return *new (_buf) T(static_cast<A&&>(a)...); }
    void reset() { if (_live) { get().~T(); _live = false; } }
    bool live() const { return _live; }
    T& get() { return *reinterpret_cast<T*>(_buf); }
    const T& get() const { return *reinterpret_cast<const T*>(_buf); }
  };

  enum : uint8_t { REQ_NONE = 0, REQ_QUERY, REQ_DEPOSIT, REQ_STATUS, REQ_REGISTER, REQ_FETCH };
  struct Pending {                  // REQ waiting for its RESPONSE; tag = REQ timestamp
    uint8_t  kind;
    uint8_t  n;
    uint32_t tag;
    uint8_t  peer[6];               // who answers: contact or mailbox
    uint8_t  contact[6];            // contact the REQ is about (STATUS, REGISTER: Alice)
    union { QueryItem q[MAX_BATCH]; uint8_t h[MAX_BATCH][8]; } asked;
  };
  static const uint8_t PENDING_MAX = 16;
  struct MbxGap { uint8_t prefix[6]; uint32_t last; bool used; };

  Late<RecordFile>   _f_meta, _f_contacts, _f_outbox, _f_watch, _f_inbox, _f_reg, _f_mbx;
  Late<ContactTable> _contacts;
  Late<Clock>        _clock;
  Late<Inbox>        _inbox;
  Late<Outbox>       _outbox;
  Late<Fetcher>      _fetcher;
  bool     _ready = false;          // begin() succeeded
  bool     _enabled = true;         // setEnabled(); RDM runs only when both are true
  uint32_t _now = 0;                // RDM time of the latest now()
  bool     _has_own_mbx = false;
  uint8_t  _own_mbx[32];
  uint8_t  _k_owner[16];
  uint8_t  _hidden[RDM_HIDDEN_PEERS][32];
  uint8_t  _n_hidden = 0;
  Pending  _pending[PENDING_MAX];
  uint8_t  _next_pending = 0;
  MbxGap   _gap[RDM_HIDDEN_PEERS];

  uint32_t now();
  void     shutdown();
  bool     loadOwnMailbox();
  bool     rebuildHiddenPeers();   // true: the set changed
  bool     contactMailbox(const uint8_t contact_prefix[6], uint8_t mbx_pub[32], uint8_t token[8]);
  bool     reqAllowed(const uint8_t mbx_pub[32]);
  void     noteReq(const uint8_t mbx_pub[32]);
  Pending* addPending(uint8_t kind, uint32_t tag, const uint8_t peer_pub[32]);
  bool     sendTracked(uint8_t kind, const uint8_t peer_pub[32], const uint8_t* body, size_t len, bool flood,
                       uint32_t& est_timeout_ms, Pending** out);
  void     onFetchResponse(const uint8_t* body, size_t len);
  void     maybeSendMbxInfo(const uint8_t sender_pub[32]);
  size_t   buildMbxCtrl(uint8_t* out, const uint8_t contact_pub[32]);

  // OutboxHost
  void selfPub(uint8_t pub_out[32]) override;
  uint32_t random32() override;
  bool contactPub(const uint8_t pub_prefix[6], uint8_t pub_out[32]) override;
  bool hasDirectPath(const uint8_t pub_prefix[6]) override;
  bool txIdle() override;
  bool sendDm(const OutEntry& e, uint8_t attempt, bool flood, uint32_t& est_timeout_ms) override;
  bool sendReceiptQuery(const uint8_t pub_prefix[6], const QueryItem* q, uint8_t n, bool flood,
                        uint32_t& est_timeout_ms) override;
  bool sendDeposit(const OutEntry& e, uint8_t attempt, uint8_t pkt_hash_out[8], uint32_t& est_timeout_ms) override;
  bool sendStatus(const uint8_t pub_prefix[6], const uint8_t (*hashes)[8], uint8_t n, uint32_t& est_timeout_ms) override;
  bool sendRegister(const uint8_t pub_prefix[6], uint32_t& est_timeout_ms) override;
  bool pushUserStatus(const OutEntry& e, UserStatus s) override;
  bool pushSendConfirmed(const OutEntry& e) override;
  // InboxHost
  void onInboxChanged() override;
  bool sendAckS(const uint8_t sender_prefix[6], const uint8_t ack_s[6]) override;
  void onReportQueued() override;
  // FetcherHost
  bool ownMailbox(uint8_t mbx_pub_out[32]) override;
  bool canFetch() override;
  bool sendFetch(uint8_t flags, uint32_t store_id, const Report* r, uint8_t n, uint32_t& est_timeout_ms) override;
};

}
