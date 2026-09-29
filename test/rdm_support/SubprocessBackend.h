#pragma once

#include <helpers/rdm/MailboxCore.h>

#include <deque>
#include <functional>
#include <string>
#include <sys/types.h>
#include <vector>

// rdm::MailboxBackend on the real Pi daemon: examples/mailbox_server/mbxd/mbxd.py as a child process
// (`serve --stdio --fake-clock`), speaking the line protocol of 06 par. 3.16 like SerialPiBackend does on the device.
// Lets simulator scenarios (S07) run against mbxd instead of MemMailboxBackend.
//
// Determinism: after each request a read-only STAT for an all-zero hash follows as a marker, and the backend reads
// the pipe up to the marker's reply. Every reply of a request is therefore queued before the call returns, whatever
// the host's scheduling, and poll() hands it out in simulated time. A request mbxd rejects as malformed gets no reply,
// exactly as on the serial line. The Pi clock follows the simulation: `@MBX TIME` carries clock() before every request.
class SubprocessBackend : public rdm::MailboxBackend {
public:
  using ClockFn = std::function<uint32_t()>;

  struct Owner {
    uint8_t pub[32];
    uint8_t k_owner[16];
    uint8_t ttl_days = 7, sync_days = 30, quota = 20;
  };

  // mbxd_path: mbxd.py; db_path: its SQLite file (created by owner-add or on start). clock: Pi unix time now.
  SubprocessBackend(std::string mbxd_path, std::string db_path, ClockFn clock);
  ~SubprocessBackend() override;

  // mbxd.py from $RDM_MBXD, else examples/mailbox_server/mbxd/mbxd.py below the working directory; "" if missing.
  static std::string locateMbxd();

  // `mbxd --db <db> owner-add ...` as the Pi admin would; before start(), or restart() afterwards (06 par. 3.16).
  bool ownerAdd(const Owner& o);
  bool start();                       // spawn `serve --stdio --fake-clock`; false if python3 or mbxd fails
  void stop();                        // close stdin and reap the child
  bool restart() { stop(); return start(); }
  bool running() const { return _pid > 0; }

  // Radio boot: `@MBX HELLO`; mbxd answers with the ACL, mbx.ready and mbx.time. Resets ready() until then.
  void hello();

  // MailboxBackend
  bool store(uint32_t id, const uint8_t owner[4], const uint8_t sender_pub[32], const uint8_t pkt_hash[8],
             const uint8_t* payload, uint8_t len) override;
  bool reg(uint32_t id, const uint8_t sender_pub[32], const uint8_t owner[4], const uint8_t token[8]) override;
  bool fetch(uint32_t id, const uint8_t client_pub[32], uint8_t flags, uint32_t store_id, const rdm::Report* r,
             uint8_t n) override;
  bool stat(uint32_t id, const uint8_t sender_pub[32], const uint8_t (*hashes)[8], uint8_t n) override;
  bool poll(rdm::BackendReply& out) override;
  bool pollAcl(uint8_t pub_out[32], bool& is_owner, uint8_t owner_out[4]) override;
  bool ready() override { return _ready; }
  uint32_t backendTime() override;

  size_t pendingReplies() const { return _replies.size(); }
  // Every line in both directions, in order: "> " radio to Pi, "< " Pi to radio. Markers are left out.
  const std::vector<std::string>& lineLog() const { return _log; }
  // Number of "< mbx.<kind> <id> <code>" lines so far with this kind and code, e.g. ("store", 0x00).
  size_t countReplies(const char* kind, uint8_t code) const;
  const std::string& lastError() const { return _error; }

private:
  struct Acl { uint8_t pub[32]; bool is_owner; uint8_t owner[4]; };

  std::string _mbxd, _db;
  ClockFn _clock;
  pid_t _pid = -1;
  int _to_child = -1, _from_child = -1;
  std::string _rx;                    // bytes read but not yet split into lines
  uint32_t _marker = 0xF0000000u;     // ids above MailboxCore's counter range
  uint32_t _time_sent = 0;            // last `@MBX TIME` value; a fresh mbxd starts its fake clock at 0
  bool _ready = false;
  bool _has_time = false;
  uint32_t _mbx_time = 0, _mbx_time_at = 0;   // last mbx.time and clock() when it arrived
  std::deque<rdm::BackendReply> _replies;
  std::deque<Acl> _acl;
  std::vector<std::string> _log;
  std::string _error;

  bool request(const std::string& line);
  bool writeLine(const std::string& line);
  bool readLine(std::string& line, int timeout_ms);
  void handleLine(const std::string& line);
  bool fail(const std::string& why);
};
