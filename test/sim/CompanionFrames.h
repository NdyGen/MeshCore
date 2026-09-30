#pragma once

// Companion protocol frames as the simulated apps build and read them. The upstream codes are file-local in
// examples/companion_radio/MyMesh.cpp, so they are defined once here; with WITH_RELIABLE_DM the codes that
// RdmCompanionProto.h already has come from there, so a test can use both namespaces without ambiguity.

#include <helpers/TxtDataHelpers.h>
#include <helpers/rdm/RdmBytes.h>

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <string>
#include <vector>

#ifdef WITH_RELIABLE_DM
  #include "RdmCompanionProto.h"
#endif

namespace sim { namespace frames {

constexpr uint8_t CMD_APP_START         = 1;
constexpr uint8_t CMD_SEND_TXT_MSG      = 2;
constexpr uint8_t CMD_GET_CONTACTS      = 4;
constexpr uint8_t CMD_SET_DEVICE_TIME   = 6;
constexpr uint8_t CMD_SEND_SELF_ADVERT  = 7;
constexpr uint8_t CMD_SYNC_NEXT_MESSAGE = 10;
constexpr uint8_t CMD_RESET_PATH        = 13;
constexpr uint8_t CMD_DEVICE_QUERY      = 22;

constexpr uint8_t RESP_CODE_OK               = 0;
constexpr uint8_t RESP_CODE_ERR              = 1;
constexpr uint8_t RESP_CODE_END_OF_CONTACTS  = 4;
constexpr uint8_t RESP_CODE_SENT             = 6;
constexpr uint8_t RESP_CODE_NO_MORE_MESSAGES = 10;

constexpr uint8_t PUSH_CODE_MSG_WAITING = 0x83;
constexpr uint8_t ERR_CODE_ILLEGAL_ARG  = 6;
constexpr uint8_t APP_PROTOCOL_VER      = 3;

#ifdef WITH_RELIABLE_DM
using rdm::companion::RESP_CODE_CONTACT_MSG_RECV;
using rdm::companion::RESP_CODE_CHANNEL_MSG_RECV;
using rdm::companion::RESP_CODE_CONTACT_MSG_RECV_V3;
using rdm::companion::RESP_CODE_CHANNEL_MSG_RECV_V3;
using rdm::companion::PUSH_CODE_SEND_CONFIRMED;

using rdm::companion::CMD_RDM_ENABLE;
using rdm::companion::CMD_RDM_SET_MAILBOX;
using rdm::companion::CMD_RDM_LIST_OUTBOX;
using rdm::companion::RESP_CODE_RDM_OUTBOX_START;
using rdm::companion::RESP_CODE_RDM_OUTBOX_ENTRY;
using rdm::companion::RESP_CODE_RDM_OUTBOX_END;
using rdm::companion::PUSH_CODE_RDM_STATUS;
#else
constexpr uint8_t RESP_CODE_CONTACT_MSG_RECV    = 7;
constexpr uint8_t RESP_CODE_CHANNEL_MSG_RECV    = 8;
constexpr uint8_t RESP_CODE_CONTACT_MSG_RECV_V3 = 16;
constexpr uint8_t RESP_CODE_CHANNEL_MSG_RECV_V3 = 17;
constexpr uint8_t PUSH_CODE_SEND_CONFIRMED      = 0x82;
#endif

// CMD_SEND_TXT_MSG: code | txt_type | attempt | ts(4) | pub_prefix(6) | text
inline std::vector<uint8_t> buildSendTxt(const uint8_t pub_prefix[6], uint32_t ts, uint8_t attempt,
                                         const std::string& text, uint8_t txt_type = TXT_TYPE_PLAIN) {
  std::vector<uint8_t> f = {CMD_SEND_TXT_MSG, txt_type, attempt, 0, 0, 0, 0};
  rdm::put32(&f[3], ts);
  f.insert(f.end(), pub_prefix, pub_prefix + 6);
  f.insert(f.end(), text.begin(), text.end());
  return f;
}

// RESP_CODE_SENT: code | flood(1) | expected_ack(4) | est_timeout_ms(4)
struct SentReply {
  bool     flood;
  uint32_t expected_ack;
  uint32_t est_timeout_ms;
};

inline bool decodeSent(const std::vector<uint8_t>& f, SentReply& out) {
  if (f.size() < 10 || f[0] != RESP_CODE_SENT) return false;
  out.flood = f[1] == 1;
  out.expected_ack = rdm::get32(&f[2]);
  out.est_timeout_ms = rdm::get32(&f[6]);
  return true;
}

// RESP_CODE_CONTACT_MSG_RECV(_V3): code | [snr | 2 reserved] | pub_prefix(6) | path_len | txt_type | ts(4)
// | [signature(4) for TXT_TYPE_SIGNED_PLAIN] | text
struct ContactMsg {
  uint8_t     sender_prefix[6];
  uint8_t     path_len;
  uint8_t     txt_type;
  uint32_t    ts;
  std::string text;
};

inline bool decodeContactMsg(const std::vector<uint8_t>& f, ContactMsg& out) {
  if (f.empty() || (f[0] != RESP_CODE_CONTACT_MSG_RECV && f[0] != RESP_CODE_CONTACT_MSG_RECV_V3)) return false;
  size_t i = f[0] == RESP_CODE_CONTACT_MSG_RECV_V3 ? 4 : 1;
  if (f.size() < i + 12) return false;
  memcpy(out.sender_prefix, &f[i], 6);
  out.path_len = f[i + 6];
  out.txt_type = f[i + 7];
  out.ts = rdm::get32(&f[i + 8]);
  i += 12;
  if (out.txt_type == TXT_TYPE_SIGNED_PLAIN) i += 4;
  if (i > f.size()) return false;
  out.text.assign(f.begin() + (long)i, f.end());
  return true;
}

}}
