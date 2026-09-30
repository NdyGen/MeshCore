#pragma once

#include "RdmConfig.h"
#include "RdmContacts.h"
#include "RdmStorage.h"
#include "RdmTypes.h"

namespace rdm {

struct InRecord {                   // inbox payload, 188 bytes (packed serialisation)
  uint8_t  sender_prefix[6];
  uint32_t ts;
  uint32_t recv_time;
  uint8_t  txt_type;
  uint8_t  path_len;                // 0xFF = direct
  int8_t   snr_x4;
  uint8_t  flags;                   // IF_VIA_MAILBOX, IF_SENDER_CAP
  uint8_t  mbx_hash[8];
  uint8_t  text_len;
  char     text[INBOX_TEXT_MAX + 1];
};
struct RegRecord {                  // register payload, 36 bytes (packed serialisation)
  uint8_t  sender_prefix[6];
  uint32_t ts;
  uint8_t  key[4];
  uint8_t  ack_r[4];
  uint8_t  ack_s[6];
  uint8_t  state;                   // 0 ON_RADIO, 1 SYNCED
  uint8_t  flags;                   // IF_VIA_MAILBOX, IF_SENDER_CAP, RF_REPORT_PENDING, RF_REPORT_DUP, RF_MBX_OUTCOME
  uint8_t  mbx_hash[8];
  uint16_t inbox_slot;              // 0xFFFF after sync
};
enum : uint8_t { IF_VIA_MAILBOX = 0x01, IF_SENDER_CAP = 0x02, RF_REPORT_PENDING = 0x04,
                 RF_REPORT_DUP = 0x08,      // pending report is DUPLICATE instead of ON_RADIO
                 RF_MBX_OUTCOME = 0x10 };   // mbx_hash belongs to a mailbox copy that still needs its outcome

struct RecvInput {
  const uint8_t* sender_pub;        // 32
  uint32_t ts; uint8_t flags; uint8_t txt_type;
  const char* text; uint8_t text_len;
  bool sender_cap;
  bool via_mailbox; const uint8_t* mbx_hash;   // 8 bytes when via_mailbox
  uint8_t path_len; int8_t snr_x4;
};
struct RecvDecision {
  RecvResult result;
  bool send_ack_r;                  // direct to sender (false for mailbox copies and for INBOX_FULL/STORE_ERROR)
  bool send_ack_s;                  // duplicate of an already synced message, sender has cap, not via mailbox
  bool cap_byte;                    // ACK_R with CAP_BYTE
  uint8_t ack_r[4]; uint8_t ack_s[6];
};

class InboxHost {
public:
  virtual ~InboxHost() {}
  virtual void onInboxChanged() = 0;                                       // PUSH_CODE_MSG_WAITING
  virtual bool sendAckS(const uint8_t sender_prefix[6], const uint8_t ack_s[6]) = 0;
  virtual void onReportQueued() = 0;                                       // Fetcher: FETCH after sync
};

class Inbox {
public:
  static constexpr uint16_t    RECORD_SIZE = 188;          // InRecord, packed
  static constexpr const char* PATH = "/rdm/inbox";
  static constexpr uint16_t    REG_RECORD_SIZE = 36;       // RegRecord, packed
  static constexpr const char* REG_PATH = "/rdm/register";

  Inbox(RecordFile& inbox, RecordFile& reg, ContactTable& contacts, InboxHost& host);
  bool         begin(bool& recreated_out);
  RecvDecision onMessage(const RecvInput& in, uint32_t now);
  bool         nextForApp(InRecord& out, uint16_t& slot_out);   // oldest unsynced, not yet offered on this connection
  void         onHandedToApp(uint16_t slot);
  void         onNextSyncRequest(uint32_t now);                  // confirms the last offered slot: SYNCED
  void         onClientDisconnected();
  QueryReply   query(const uint8_t sender_prefix[6], const QueryItem& q);
  uint8_t      pendingReports(Report* out, uint8_t max);
  void         onReportsConfirmed(const Report* sent, uint8_t n_ok);
  void         onUndecryptable(const uint8_t mbx_hash[8]);       // queues report UNDECRYPTABLE for this mailbox copy
  uint16_t     freeSlots() const;

private:
  RecordFile&   _inbox;
  RecordFile&   _reg;
  ContactTable& _contacts;
  InboxHost&    _host;

  struct RegIdx { uint32_t ts; uint8_t key[4]; uint16_t inbox_slot; uint8_t flags; };
  struct InIdx  { uint32_t recv_time; uint8_t sender[6]; uint8_t flags; };
  RegIdx   _reg_idx[RDM_REGISTER_SLOTS_MAX];
  InIdx    _in_idx[RDM_INBOX_SLOTS_MAX];
  Report   _extra[MAX_BATCH];       // reports without a register row to carry them (RAM only)
  uint8_t  _n_extra;
  uint16_t _reg_slots;
  uint16_t _in_slots;
  uint16_t _handed;                 // inbox slot offered last on this connection, 0xFFFF: none

  RecvResult store(const RecvInput& in, const uint8_t key[4], const RecvDecision& d, uint32_t now);
  int  findReg(const uint8_t prefix[6], uint32_t ts, const uint8_t key[4], RegRecord* out);
  int  findRegForInbox(uint16_t inbox_slot) const;
  int  freeRegSlot() const;
  int  evictReg();
  int  freeInboxSlot() const;
  uint16_t unsyncedFrom(const uint8_t prefix[6]) const;
  bool writeReg(uint16_t slot, const RegRecord& r);
  void queueMailboxReport(uint16_t slot, const RegRecord& r, const uint8_t mbx_hash[8]);
  void addExtra(const Report& rep);
  void dropInboxSlot(uint16_t slot);
  void reconcile();
  void resetWatermarks();
};

}
