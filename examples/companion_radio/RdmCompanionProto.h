#pragma once

// Companion frames of the reliable-DM fork (06 par. 3.14), as pure functions so both the firmware (MyMesh) and
// test clients use the same encoding. Header-only on purpose: the companion envs build examples/companion_radio/*.cpp,
// so a separate .cpp would add an object to builds without WITH_RELIABLE_DM.

#include <helpers/rdm/RdmBytes.h>
#include <helpers/rdm/RdmInbox.h>
#include <helpers/rdm/RdmOutbox.h>
#include <helpers/rdm/RdmTypes.h>
#include <Utils.h>

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace rdm { namespace companion {

constexpr uint8_t CMD_RDM_ENABLE       = 0xC0;
constexpr uint8_t CMD_RDM_SET_MAILBOX  = 0xC1;
constexpr uint8_t CMD_RDM_LIST_OUTBOX  = 0xC2;
constexpr uint8_t RESP_CODE_RDM_OUTBOX_START = 0x40;
constexpr uint8_t RESP_CODE_RDM_OUTBOX_ENTRY = 0x41;
constexpr uint8_t RESP_CODE_RDM_OUTBOX_END   = 0x42;
constexpr uint8_t PUSH_CODE_RDM_STATUS = 0x91;
constexpr uint8_t PROTO_VERSION        = 1;

// upstream codes these frames share their layout with (file-local in MyMesh.cpp)
constexpr uint8_t RESP_CODE_CONTACT_MSG_RECV    = 7;
constexpr uint8_t RESP_CODE_CHANNEL_MSG_RECV    = 8;
constexpr uint8_t RESP_CODE_CONTACT_MSG_RECV_V3 = 16;
constexpr uint8_t RESP_CODE_CHANNEL_MSG_RECV_V3 = 17;
constexpr uint8_t PUSH_CODE_SEND_CONFIRMED      = 0x82;

constexpr size_t STATUS_FRAME_LEN   = 20;
constexpr size_t OUTBOX_START_LEN   = 2;
constexpr size_t OUTBOX_ENTRY_LEN   = 22;
constexpr size_t OUTBOX_END_LEN     = 1;
constexpr size_t SET_MAILBOX_LEN    = 1 + 32 + 16;
constexpr size_t SEND_CONFIRMED_LEN = 9;
constexpr size_t MAX_APP_FRAME      = 176;   // MAX_FRAME_SIZE (BaseSerialInterface.h)

// ---- PUSH_CODE_RDM_STATUS: 0x91 | app_ack(4) | UserStatus(1) | K(4) | ts(4) | pub_prefix(6) ----

struct StatusPush {
  uint32_t   app_ack;
  UserStatus status;
  uint8_t    key[4];
  uint32_t   ts;
  uint8_t    pub_prefix[6];
};

inline size_t encodeStatus(uint8_t out[STATUS_FRAME_LEN], const StatusPush& p) {
  out[0] = PUSH_CODE_RDM_STATUS;
  put32(&out[1], p.app_ack);
  out[5] = (uint8_t)p.status;
  memcpy(&out[6], p.key, 4);
  put32(&out[10], p.ts);
  memcpy(&out[14], p.pub_prefix, 6);
  return STATUS_FRAME_LEN;
}

inline bool decodeStatus(const uint8_t* f, size_t len, StatusPush& p) {
  if (len < STATUS_FRAME_LEN || f[0] != PUSH_CODE_RDM_STATUS || f[5] > (uint8_t)UserStatus::NO_OUTBOX) return false;
  p.app_ack = get32(&f[1]);
  p.status = (UserStatus)f[5];
  memcpy(p.key, &f[6], 4);
  p.ts = get32(&f[10]);
  memcpy(p.pub_prefix, &f[14], 6);
  return true;
}

// ---- CMD_RDM_LIST_OUTBOX reply, like CMD_GET_CONTACTS: START | ENTRY per entry | END ----
// START: 0x40 | count;  ENTRY: 0x41 | idx | pub_prefix(6) | ts(4) | app_ack(4) | UserStatus | OutState | K(4);  END: 0x42

struct OutboxRow {
  uint8_t    idx;
  uint8_t    pub_prefix[6];
  uint32_t   ts, app_ack;
  UserStatus status;
  OutState   state;
  uint8_t    key[4];
};

inline OutboxRow outboxRow(const OutEntry& e, uint8_t idx) {
  OutboxRow r;
  r.idx = idx;
  memcpy(r.pub_prefix, e.pub_prefix, 6);
  r.ts = e.ts;
  r.app_ack = e.app_ack;
  r.status = toUserStatus(e.state);
  r.state = e.state;
  memcpy(r.key, e.key, 4);
  return r;
}

inline size_t encodeOutboxStart(uint8_t out[OUTBOX_START_LEN], uint8_t count) {
  out[0] = RESP_CODE_RDM_OUTBOX_START;
  out[1] = count;
  return OUTBOX_START_LEN;
}

inline bool decodeOutboxStart(const uint8_t* f, size_t len, uint8_t& count) {
  if (len < OUTBOX_START_LEN || f[0] != RESP_CODE_RDM_OUTBOX_START) return false;
  count = f[1];
  return true;
}

inline size_t encodeOutboxEntry(uint8_t out[OUTBOX_ENTRY_LEN], const OutboxRow& r) {
  out[0] = RESP_CODE_RDM_OUTBOX_ENTRY;
  out[1] = r.idx;
  memcpy(&out[2], r.pub_prefix, 6);
  put32(&out[8], r.ts);
  put32(&out[12], r.app_ack);
  out[16] = (uint8_t)r.status;
  out[17] = (uint8_t)r.state;
  memcpy(&out[18], r.key, 4);
  return OUTBOX_ENTRY_LEN;
}

inline bool decodeOutboxEntry(const uint8_t* f, size_t len, OutboxRow& r) {
  if (len < OUTBOX_ENTRY_LEN || f[0] != RESP_CODE_RDM_OUTBOX_ENTRY) return false;
  r.idx = f[1];
  memcpy(r.pub_prefix, &f[2], 6);
  r.ts = get32(&f[8]);
  r.app_ack = get32(&f[12]);
  r.status = (UserStatus)f[16];
  r.state = (OutState)f[17];
  memcpy(r.key, &f[18], 4);
  return true;
}

inline size_t encodeOutboxEnd(uint8_t out[OUTBOX_END_LEN]) {
  out[0] = RESP_CODE_RDM_OUTBOX_END;
  return OUTBOX_END_LEN;
}

// ---- commands ----

inline size_t buildEnable(uint8_t out[2]) {
  out[0] = CMD_RDM_ENABLE;
  out[1] = PROTO_VERSION;
  return 2;
}

inline bool parseEnable(const uint8_t* f, size_t len, uint8_t& version) {
  if (len < 2 || f[0] != CMD_RDM_ENABLE) return false;
  version = f[1];
  return true;
}

enum class MailboxCmd : uint8_t { SET, CLEAR, INVALID };

// mbx_pub and k_owner both nullptr: no own mailbox
inline size_t buildSetMailbox(uint8_t* out, const uint8_t* mbx_pub, const uint8_t* k_owner) {
  out[0] = CMD_RDM_SET_MAILBOX;
  if (!mbx_pub || !k_owner) return 1;
  memcpy(&out[1], mbx_pub, 32);
  memcpy(&out[33], k_owner, 16);
  return SET_MAILBOX_LEN;
}

inline MailboxCmd parseSetMailbox(const uint8_t* f, size_t len, const uint8_t*& mbx_pub, const uint8_t*& k_owner) {
  mbx_pub = nullptr;
  k_owner = nullptr;
  if (len == 0 || f[0] != CMD_RDM_SET_MAILBOX) return MailboxCmd::INVALID;
  if (len == 1) return MailboxCmd::CLEAR;
  if (len != SET_MAILBOX_LEN) return MailboxCmd::INVALID;
  mbx_pub = &f[1];
  k_owner = &f[33];
  return MailboxCmd::SET;
}

inline size_t buildListOutbox(uint8_t out[1]) {
  out[0] = CMD_RDM_LIST_OUTBOX;
  return 1;
}

// ---- frames the standard app already knows ----

// Inbox record as reply to CMD_SYNC_NEXT_MESSAGE, byte for byte like MyMesh::queueMessage for a DM.
inline size_t encodeContactMsg(uint8_t* out, size_t max, const InRecord& r, uint8_t app_ver) {
  size_t i = 0;
  if (app_ver >= 3) {
    out[i++] = RESP_CODE_CONTACT_MSG_RECV_V3;
    out[i++] = (uint8_t)r.snr_x4;
    out[i++] = 0;
    out[i++] = 0;
  } else {
    out[i++] = RESP_CODE_CONTACT_MSG_RECV;
  }
  memcpy(&out[i], r.sender_prefix, 6);
  i += 6;
  out[i++] = r.path_len;
  out[i++] = r.txt_type;
  put32(&out[i], r.ts);
  i += 4;
  size_t tlen = r.text_len;
  if (i + tlen > max) tlen = max - i;
  memcpy(&out[i], r.text, tlen);
  return i + tlen;
}

inline size_t encodeSendConfirmed(uint8_t out[SEND_CONFIRMED_LEN], uint32_t app_ack, uint32_t trip_ms) {
  out[0] = PUSH_CODE_SEND_CONFIRMED;
  put32(&out[1], app_ack);
  put32(&out[5], trip_ms);
  return SEND_CONFIRMED_LEN;
}

// ---- local status channel "rdm-status" (RDM_STATUS_CHANNEL) ----

constexpr const char* STATUS_CHANNEL_NAME = "rdm-status";

inline const char* statusName(UserStatus s) {
  switch (s) {
    case UserStatus::QUEUED:         return "queued";
    case UserStatus::CUSTODY:        return "in mailbox";
    case UserStatus::ON_RADIO:       return "on radio";
    case UserStatus::DELIVERED:      return "delivered";
    case UserStatus::ON_RADIO_FINAL: return "on radio (no read receipt)";
    case UserStatus::REJECTED:       return "rejected by mailbox";
    case UserStatus::EXPIRED:        return "expired";
    case UserStatus::SYNC_EXPIRED:   return "not read in time";
    case UserStatus::TOO_BIG:        return "too long, sent without outbox";
    case UserStatus::NO_OUTBOX:      return "outbox full, sent without outbox";
  }
  return "?";
}

// "<name>: <status> (sent HH:MM UTC)"; the send time tells which message it is, the text is not at hand here.
inline size_t formatStatusLine(char* out, size_t max, const char* name, UserStatus s, uint32_t msg_ts) {
  uint32_t day_s = msg_ts % 86400;
  int n = snprintf(out, max, "%s: %s (sent %02u:%02u UTC)", name, statusName(s), (unsigned)(day_s / 3600),
                   (unsigned)(day_s / 60 % 60));
  if (n < 0) return 0;
  return (size_t)n < max ? (size_t)n : max - 1;
}

// Channel message as MyMesh::onChannelMessageRecv queues it; path_len 0xFF (local), SNR 0.
inline size_t encodeChannelMsg(uint8_t* out, size_t max, uint8_t app_ver, uint8_t channel_idx, uint32_t ts,
                               const char* text) {
  size_t i = 0;
  if (app_ver >= 3) {
    out[i++] = RESP_CODE_CHANNEL_MSG_RECV_V3;
    out[i++] = 0;
    out[i++] = 0;
    out[i++] = 0;
  } else {
    out[i++] = RESP_CODE_CHANNEL_MSG_RECV;
  }
  out[i++] = channel_idx;
  out[i++] = 0xFF;
  out[i++] = 0;   // TXT_TYPE_PLAIN
  put32(&out[i], ts);
  i += 4;
  size_t tlen = strlen(text);
  if (i + tlen > max) tlen = max - i;
  memcpy(&out[i], text, tlen);
  return i + tlen;
}

// Per-device 128-bit key, so a message typed into this channel by mistake cannot be read by anyone else.
inline void statusChannelSecret(uint8_t secret_out[32], const uint8_t self_pub[32]) {
  memset(secret_out, 0, 32);
  mesh::Utils::sha256(secret_out, 16, self_pub, 32, (const uint8_t*)STATUS_CHANNEL_NAME,
                      (int)strlen(STATUS_CHANNEL_NAME));
}

// ---- pushes to the app, sent from the main loop while the link is not write-busy ----
// The 0x91 replay on CMD_RDM_ENABLE exceeds the ESP32 BLE send queue of 4 frames, and the first 0x91 of a message
// must follow RESP_CODE_SENT (G1), which is written while the command is handled.

template <uint8_t N, uint8_t MAXLEN>
class FrameQueue {
  uint8_t _buf[N][MAXLEN];
  uint8_t _len[N];
  uint8_t _head = 0, _count = 0;

public:
  bool    empty() const { return _count == 0; }
  uint8_t size() const { return _count; }
  void    clear() { _head = _count = 0; }

  bool push(const uint8_t* f, size_t len) {
    if (_count == N || len == 0 || len > MAXLEN) return false;
    uint8_t slot = (uint8_t)((_head + _count) % N);
    memcpy(_buf[slot], f, len);
    _len[slot] = (uint8_t)len;
    _count++;
    return true;
  }

  const uint8_t* front(size_t& len) const {
    len = _len[_head];
    return _buf[_head];
  }

  void pop() {
    if (_count == 0) return;
    _head = (uint8_t)((_head + 1) % N);
    _count--;
  }

  // Status frames of this message that went out without an app_ack get the one RESP_CODE_SENT carried.
  void fillAppAck(const uint8_t pub_prefix[6], uint32_t ts, uint32_t app_ack) {
    for (uint8_t k = 0; k < _count; k++) {
      uint8_t* f = _buf[(_head + k) % N];
      if (_len[(_head + k) % N] == STATUS_FRAME_LEN && f[0] == PUSH_CODE_RDM_STATUS && get32(&f[1]) == 0 &&
          get32(&f[10]) == ts && memcmp(&f[14], pub_prefix, 6) == 0) {
        put32(&f[1], app_ack);
      }
    }
  }
};

}}
