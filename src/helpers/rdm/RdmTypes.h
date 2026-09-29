#pragma once

#include <stddef.h>
#include <stdint.h>

namespace rdm {

constexpr uint8_t TXT_TYPE_RDM_CTRL     = 8;
constexpr uint8_t CAP_BYTE              = 0x81;
constexpr uint8_t CTRL_MBX_INFO         = 0x01;
constexpr uint8_t CTRL_MBX_REVOKE       = 0x02;
constexpr uint8_t CTRL_VERSION          = 1;
constexpr uint8_t REQ_DEPOSIT           = 0x41;
constexpr uint8_t REQ_FETCH             = 0x42;
constexpr uint8_t REQ_STATUS            = 0x44;
constexpr uint8_t REQ_RECEIPT_QUERY     = 0x45;
constexpr uint8_t REG_MARKER0           = 0xFF;
constexpr uint8_t REG_MARKER1           = 'M';
constexpr uint8_t REG_VERSION           = 1;
constexpr uint8_t FETCH_FLAG_NO_PAYLOAD = 0x02;
constexpr uint8_t FW_ATTEMPT_BASE       = 252;   // + class, descending in steps of 4 (03 par. 1d)
constexpr size_t  MAX_TEXT              = 157;   // fork DM with trailer
constexpr size_t  INBOX_TEXT_MAX        = 160;   // upstream MAX_TEXT_LEN: stock senders may use all of it
constexpr size_t  MAX_TEXT_MAILBOX      = 152;   // mailbox copy
constexpr size_t  MAX_INNER_PAYLOAD     = 164;   // dest|src|MAC|ciphertext of a mailbox copy
constexpr size_t  MAX_BATCH             = 8;

enum class OutState : uint8_t {
  NEW = 0, WAIT_ACK = 1, RETRY = 2, DEPOSITING = 3, REGISTER = 4, CUSTODY = 5,
  ON_RADIO = 6, ON_RADIO_FINAL = 7, DELIVERED = 8, REJECTED = 9, EXPIRED = 10, SYNC_EXPIRED = 11
};
enum class UserStatus : uint8_t {        // value in PUSH_CODE_RDM_STATUS
  QUEUED = 0, CUSTODY = 1, ON_RADIO = 2, DELIVERED = 3, ON_RADIO_FINAL = 4,
  REJECTED = 5, EXPIRED = 6, SYNC_EXPIRED = 7, TOO_BIG = 8, NO_OUTBOX = 9
};
enum class QueryState   : uint8_t { UNKNOWN = 0, ON_RADIO = 1, SYNCED = 2, EVICTED = 3 };
enum class ReportResult : uint8_t { ON_RADIO = 0, SYNCED = 1, DUPLICATE = 2, UNDECRYPTABLE = 3, INBOX_FULL = 4 };
enum class MbxState     : uint8_t { UNKNOWN = 0, STORED = 1, ON_RADIO = 2, DELIVERED = 3, REJECTED = 4, EXPIRED = 5, SYNC_EXPIRED = 6 };
enum class MbxCode      : uint8_t { OK = 0x00, ALREADY_STORED = 0x01, NOT_AUTH = 0x10, UNKNOWN_OWNER = 0x11,
                                    QUOTA = 0x12, NO_STORAGE = 0x13, TOO_BIG = 0x14, RATE_LIMITED = 0x15 };
enum class RecvResult   : uint8_t { NEW = 0, DUPLICATE = 1, INBOX_FULL = 2, STORE_ERROR = 3 };

struct QueryItem   { uint32_t ts; uint8_t key[4]; };
struct QueryReply  { QueryState state; uint8_t ack[6]; };      // ON_RADIO: ACK_R + 2 zero bytes; SYNCED: ACK_S
struct Report      { uint8_t pkt_hash[8]; ReportResult result; uint8_t ack[6]; };
struct StatusReply { MbxState state; uint8_t ack[6]; };

inline UserStatus toUserStatus(OutState s) {                   // 03 par. 11, G1
  switch (s) {
    case OutState::CUSTODY:        return UserStatus::CUSTODY;
    case OutState::ON_RADIO:       return UserStatus::ON_RADIO;
    case OutState::ON_RADIO_FINAL: return UserStatus::ON_RADIO_FINAL;
    case OutState::DELIVERED:      return UserStatus::DELIVERED;
    case OutState::REJECTED:       return UserStatus::REJECTED;
    case OutState::EXPIRED:        return UserStatus::EXPIRED;
    case OutState::SYNC_EXPIRED:   return UserStatus::SYNC_EXPIRED;
    default:                       return UserStatus::QUEUED;
  }
}
inline bool isFinal(OutState s) { return s >= OutState::ON_RADIO_FINAL; }

}
