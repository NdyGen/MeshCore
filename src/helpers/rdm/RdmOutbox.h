#pragma once

#include "RdmConfig.h"
#include "RdmContacts.h"
#include "RdmStorage.h"
#include "RdmTypes.h"

namespace rdm {

struct OutEntry {                   // record payload, 201 bytes (packed serialisation)
  uint8_t  pub_prefix[6];
  uint32_t ts;
  uint8_t  key[4];
  uint8_t  klass;                   // attempt & 3 of the first app attempt
  uint8_t  fw_attempt;              // next firmware attempt: 252 + klass, descending by 4, wraps to 252 + klass
  OutState state;
  uint8_t  flags;                   // OF_UNREPORTED, OF_CONFIRM_PENDING, OF_CONFIRM_SENT, OF_MBX_COPY, OF_CTRL
  uint32_t app_ack;                 // ACK_R of the latest app attempt (for SEND_CONFIRMED)
  uint8_t  pkt_hash[8];             // mailbox copy (OF_MBX_COPY)
  uint32_t created;
  uint32_t deadline;                // T_radio, T_sync or now + ttl_s of M + grace, RDM time
  uint32_t final_at;                // 0 until a final state
  uint8_t  text_len;
  char     text[MAX_TEXT + 1];      // with OF_CTRL: CTRL body from sub onwards
};
enum : uint8_t { OF_UNREPORTED = 0x01, OF_CONFIRM_PENDING = 0x02, OF_CONFIRM_SENT = 0x04, OF_MBX_COPY = 0x08, OF_CTRL = 0x10 };

class OutboxHost {
public:
  virtual ~OutboxHost() {}
  virtual void selfPub(uint8_t pub_out[32]) = 0;                                    // own key: sender of ACK_R/ACK_S/K/CTRL-ACK
  virtual uint32_t random32() = 0;                                                  // jitter (G16)
  virtual bool contactPub(const uint8_t pub_prefix[6], uint8_t pub_out[32]) = 0;   // false: contact gone
  virtual bool hasDirectPath(const uint8_t pub_prefix[6]) = 0;
  virtual bool txIdle() = 0;
  virtual bool sendDm(const OutEntry& e, uint8_t attempt, bool flood, uint32_t& est_timeout_ms) = 0;
  virtual bool sendReceiptQuery(const uint8_t pub_prefix[6], const QueryItem* q, uint8_t n, bool flood,
                                uint32_t& est_timeout_ms) = 0;
  virtual bool sendDeposit(const OutEntry& e, uint8_t attempt, uint8_t pkt_hash_out[8], uint32_t& est_timeout_ms) = 0;
  virtual bool sendStatus(const uint8_t pub_prefix[6], const uint8_t (*hashes)[8], uint8_t n, uint32_t& est_timeout_ms) = 0;
  virtual bool sendRegister(const uint8_t pub_prefix[6], uint32_t& est_timeout_ms) = 0;
  virtual bool pushUserStatus(const OutEntry& e, UserStatus s) = 0;   // true: a 0x91 client received it
  virtual bool pushSendConfirmed(const OutEntry& e) = 0;              // true: app connected
};

class Outbox {
public:
  static constexpr uint16_t    RECORD_SIZE = 201;          // OutEntry, packed
  static constexpr const char* PATH = "/rdm/outbox";
  static constexpr uint16_t    WATCH_RECORD_SIZE = 28;     // receipt watch after SYNC_EXPIRED
  static constexpr const char* WATCH_PATH = "/rdm/watch";

  enum class SendResult : uint8_t { NEW_ENTRY, EXISTING_ENTRY, NO_OUTBOX, TOO_LONG };

  Outbox(RecordFile& file, RecordFile& watch, ContactTable& contacts, OutboxHost& host);
  bool       begin(uint32_t now);                          // load, recompute ACKs, schedule boot kick
  SendResult onAppSend(const uint8_t pub_prefix[6], uint32_t ts, uint8_t attempt, const char* text, size_t text_len,
                       uint32_t now, uint32_t& app_ack_out, bool& transmit_out);
  void       onAppTransmitted(const uint8_t pub_prefix[6], uint32_t ts, uint32_t est_timeout_ms, uint32_t now);
  bool       addCtrl(const uint8_t pub_prefix[6], const uint8_t* ctrl_plain, size_t len, uint32_t now);  // MBX_INFO/REVOKE
  bool       onAck(const uint8_t* ack, uint8_t len, uint32_t now);          // 4-7 bytes; true on match
  void       onQueryReply(const uint8_t pub_prefix[6], const QueryItem* asked, const QueryReply* r, uint8_t n, uint32_t now);
  void       onDepositReply(MbxCode st, const uint8_t pkt_hash[8], uint32_t ttl_s, uint32_t now);   // matched on pkt_hash
  void       onStatusReply(const uint8_t pub_prefix[6], const uint8_t (*asked)[8], const StatusReply* r, uint8_t n, uint32_t now);
  void       onRegisterReply(const uint8_t pub_prefix[6], MbxCode st, uint32_t now);
  void       onHeard(const uint8_t pub_prefix[6], bool had_cap_trailer, uint32_t now);   // G15: clear cap without trailer
  void       onMailboxAdvert(const uint8_t mbx_pub_prefix[6], uint32_t now);
  void       onMailboxChanged(const uint8_t pub_prefix[6], uint32_t now);  // new MBX_INFO or REVOKE (G14)
  void       onClientConnected(bool rdm_client, uint32_t now);             // G5
  void       loop(uint32_t now);
  // Earliest RDM time (s) at which loop() acts (next DM/probe/query/deposit/status/register, request timeout,
  // deadline, final_at + RDM_FINAL_KEEP_S). Never earlier than now; UINT32_MAX: nothing scheduled.
  uint32_t   nextDue(uint32_t now) const;
  uint8_t    count() const;
  bool       get(uint8_t idx, OutEntry& out) const;

private:
  RecordFile&   _file;
  ContactTable& _contacts;
  OutboxHost&   _host;
  RecordFile&   _watch_file;

  enum Action : uint8_t { ACT_DM, ACT_QUERY, ACT_STATUS, ACT_DEPOSIT, ACT_REGISTER, ACT_COUNT };
  enum Req : uint8_t { REQ_NONE, REQ_DEP, REQ_REG };

  // RAM state per slot; only `e` is persistent, schedules restart from the boot kick (G6)
  struct Slot {
    OutEntry e;
    bool     used;
    uint8_t  ack_r[4][4];           // per attempt class; OF_CTRL: [0] is the CTRL ack
    uint8_t  ack_s[6];
    uint8_t  reported;              // UserStatus + 1 of the latest 0x91 attempt, 0: none yet
    bool     in_series;             // RETRY series running: its first action already scheduled (G16)
    uint8_t  req;                   // open request to the mailbox (Req)
    uint32_t wait_until;            // NEW/WAIT_ACK timeout or timeout of `req`
    uint32_t t_even;                // G15: next whole DM despite a known cap, counted from the latest DM
    uint32_t t_dm, t_query, t_status, t_deposit;
    uint32_t w_status, w_query;     // time-out of an unanswered STATUS / ON_RADIO query (G17b)
    uint32_t r_status, r_query;     // their one repeat (G17b)
    bool     st_repeat, q_repeat;   // the outstanding one is already the repeat
    bool     final_sent;            // the STATUS after the CUSTODY deadline went out (G10)
    uint8_t  dm_step, q_step, status_step, dep_step;
    uint8_t  dep_noreply;           // consecutive DEPOSITs without reply (G9)
    uint8_t  dep_attempt;
    bool     dep_attempt_valid;     // same attempt again gives the same ciphertext, so M answers ALREADY_STORED
    bool     reg_done;              // one REGISTER per deposit round (G9)
    bool     no_deposit;            // M answered TOO_BIG
    uint32_t last_heard, last_advert;
  };
  struct Peer {
    bool     used;
    uint8_t  prefix[6];
    uint8_t  direct_fails;
    bool     flooded;
    uint32_t last_flood;
  };
  struct WatchIdx {                 // RAM index of /rdm/watch (receipt watch after SYNC_EXPIRED, G2)
    bool     used;
    uint8_t  ack_s[6];
    uint32_t expires;
  };

  Slot     _slots[RDM_OUTBOX_SLOTS_MAX];
  Peer     _peers[RDM_OUTBOX_SLOTS_MAX];
  WatchIdx _watch[RDM_WATCH_SLOTS_MAX];
  uint8_t  _n_slots;
  uint8_t  _n_watch;
  bool     _tx_any;
  uint32_t _last_fw_tx;
  uint8_t  _self[32];

  uint32_t firstDelay();
  uint32_t jittered(uint32_t interval);
  void     markDm(Slot& s, uint32_t now);
  bool     persist(uint8_t i);
  void     release(uint8_t i);
  int      allocSlot();
  void     computeAcks(Slot& s);
  bool     matchAckR(const Slot& s, const uint8_t* ack) const;
  int      findMsg(const uint8_t pub_prefix[6], uint32_t ts, const uint8_t key[4]) const;
  bool     hasCap(const uint8_t pub_prefix[6]) const;
  void     setCap(const uint8_t pub_prefix[6], bool on);
  bool     canDeposit(const Slot& s) const;
  bool     mailboxMatches(const uint8_t pub_prefix[6], const uint8_t mbx_prefix[6]) const;
  Peer&    peer(const uint8_t pub_prefix[6]);
  void     resetDirectFails(const uint8_t pub_prefix[6]);

  void     setState(uint8_t i, OutState s, uint32_t now);
  void     notify(uint8_t i);
  void     confirm(uint8_t i);
  void     releaseIfReported(uint8_t i);
  void     leaveCopy(Slot& s, uint32_t now);
  void     enterRetry(uint8_t i, uint32_t now, bool start_series = true);
  void     enterOnRadio(uint8_t i, uint32_t now);
  void     enterCustody(uint8_t i, uint32_t ttl_s, uint32_t now);
  void     startDeposit(uint8_t i, bool new_round, uint32_t now);
  void     depositFailed(uint8_t i, uint32_t now);
  void     onProofRadio(uint8_t i, bool cap, uint32_t now);
  void     onProofSynced(uint8_t i, uint32_t now);
  void     onWaitTimeout(uint8_t i, uint32_t now);
  void     onReqTimeout(uint8_t i, uint32_t now);
  void     onStatusTimeout(uint8_t i, uint32_t now);
  void     onQueryTimeout(uint8_t i, uint32_t now);
  uint8_t  nextFwAttempt(Slot& s);

  uint32_t actionDue(uint8_t i, Action a) const;
  uint32_t eventDue(uint8_t i) const;
  uint32_t gapFree() const;
  bool     runAction(uint8_t i, Action a, uint32_t now);   // true: something went on air
  bool     runDm(uint8_t i, uint32_t now);
  bool     runQuery(uint8_t i, uint32_t now);
  bool     runStatus(uint8_t i, uint32_t now);
  bool     runDeposit(uint8_t i, uint32_t now);
  bool     runRegister(uint8_t i, uint32_t now);

  void     addWatch(const Slot& s, uint32_t now);
  bool     matchWatch(const uint8_t* ack_s, uint32_t now);
};

}
