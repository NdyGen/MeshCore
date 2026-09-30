#pragma once

#include <Dispatcher.h>
#include <Stream.h>
#include <helpers/rdm/mailbox/MailboxCore.h>

// MailboxBackend over the serial line to meshcore-mailboxd on the Pi (06-implementatieplan-v1.md par. 3.16).
// Requests go out as one '@MBX ...' line each; replies ('mbx.' lines) are read without blocking whenever the core
// polls. Other input is ignored, so the same port can carry debug output.
class SerialPiBackend : public rdm::MailboxBackend {
public:
  static const uint32_t PROTO = 2;   // session protocol version in '@MBX HELLO' and 'mbx.ready'

  SerialPiBackend(Stream& io, mesh::MillisecondClock& ms);

  // Sends '@MBX HELLO PROTO fw'; the Pi answers with the ACL, 'mbx.ready PROTO' and mbx.time. Repeated every 30 s
  // until some mbx.ready arrives, for a lost line; the Pi's own 'mbx.hello?' at start is answered at once.
  void begin(const char* fw_version);
  // Version the daemon announced in its last mbx.ready: 0 none yet, 1 for a bare 'mbx.ready'. Ready only when
  // it equals PROTO; otherwise the radio waits for the next 'mbx.hello?' (a daemon upgrade).
  uint32_t daemonProto();

  bool store(uint32_t id, const uint8_t owner[4], const uint8_t sender_pub[32], const uint8_t pkt_hash[8],
             const uint8_t* payload, uint8_t len) override;
  bool reg(uint32_t id, const uint8_t sender_pub[32], const uint8_t owner[4], const uint8_t token[8]) override;
  bool fetch(uint32_t id, const uint8_t client_pub[32], uint8_t flags, uint32_t store_id, const rdm::Report* r,
             uint8_t n) override;
  bool stat(uint32_t id, const uint8_t sender_pub[32], const uint8_t (*hashes)[8], uint8_t n) override;
  bool poll(rdm::BackendReply& out) override;
  bool pollAcl(uint8_t pub_out[32], bool& is_owner, uint8_t owner_out[4]) override;
  bool ready() override;
  uint32_t backendTime() override;

private:
  static const uint16_t MAX_LINE = 400;
  static const uint8_t  REPLIES = 4;
  static const uint8_t  ACLS = 16;
  static const uint32_t HELLO_RETRY_MS = 30000;

  struct Acl { uint8_t pub[32]; bool is_owner; uint8_t owner[4]; };

  Stream& _io;
  mesh::MillisecondClock& _ms;
  const char* _fw = "";
  char     _line[MAX_LINE + 1];
  uint16_t _line_len = 0;
  bool     _discarding = false;       // current input line is too long; skip to its newline
  bool     _ready = false;
  bool     _hello_pending = false;   // HELLO sent, no mbx.ready of any version since
  uint32_t _hello_ms = 0;
  uint32_t _daemon_proto = 0;
  bool     _has_time = false;
  uint32_t _time = 0, _time_ms = 0;
  rdm::BackendReply _replies[REPLIES];
  uint8_t  _reply_head = 0, _reply_count = 0;
  Acl      _acls[ACLS];
  uint8_t  _acl_head = 0, _acl_count = 0;

  void pump();
  void sendHello();
  void handleLine(char* line);
  bool parseReply(char* line, rdm::BackendReply& r);
  bool parseAcl(char* args);
  void writeLine(const char* line, size_t len);
};
