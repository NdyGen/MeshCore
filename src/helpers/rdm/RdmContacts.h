#pragma once

#include "RdmConfig.h"
#include "RdmStorage.h"

namespace rdm {

struct ContactRdm {                 // record payload, 52 bytes
  uint8_t  pub_prefix[6];
  uint8_t  flags;                   // CR_CAP, CR_HAS_MBX, CR_MBX_INFO_SENT
  uint8_t  reserved;
  uint32_t watermark;               // highest evicted sender ts (receiver side, I7)
  uint8_t  mbx_pub[32];             // mailbox of this contact (sender side)
  uint8_t  token[8];                // token_B for that mailbox
};
enum : uint8_t { CR_CAP = 0x01, CR_HAS_MBX = 0x02, CR_MBX_INFO_SENT = 0x04 };

class ContactTable {
public:
  explicit ContactTable(RecordFile& file);
  bool        begin();
  ContactRdm* find(const uint8_t pub_prefix[6]);
  ContactRdm* findOrAdd(const uint8_t pub_prefix[6]);   // evicts: oldest without CR_HAS_MBX and watermark 0
  bool        save(const ContactRdm& c);
  ContactRdm* findByMailbox(const uint8_t mbx_pub_prefix[6]);
  uint16_t    count() const;                           // slots, used or not
  ContactRdm* at(uint16_t i);                          // nullptr for an empty slot

private:
  RecordFile& _file;
  ContactRdm  _c[RDM_CONTACT_SLOTS_MAX];
  uint32_t    _last_use[RDM_CONTACT_SLOTS_MAX];   // 0: slot free
  uint32_t    _use_clock = 0;
  uint16_t    _n = 0;

  int  indexOf(const uint8_t pub_prefix[6]) const;
  int  slotFor(const uint8_t pub_prefix[6]) const;    // existing, free or evictable; -1 if none
  bool store(int idx, const ContactRdm& c);
};

}
