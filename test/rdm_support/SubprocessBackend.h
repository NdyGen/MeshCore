#pragma once

#include <SerialPiBackend.h>
#include <Stream.h>
#include <helpers/rdm/mailbox/MailboxCore.h>

#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <sys/types.h>
#include <vector>

// rdm::MailboxBackend on the real Pi daemon (examples/mailbox_server/meshcore-mailboxd) as a child process
// (`serve --stdio --fake-clock`), with the firmware's own SerialPiBackend speaking the line protocol of 06 par. 3.16
// over a Stream on the pipes. Lets simulator scenarios (S07) run against the daemon instead of MemMailboxBackend,
// through the parser the mailbox radio uses.
//
// Determinism: after every request line a read-only STAT for an all-zero hash follows as a marker, and the stream
// reads the pipe up to the marker's reply before the write returns. Every reply is therefore in the stream before the
// call returns, whatever the host's scheduling, and poll() hands it out in simulated time. Lines the daemon sends on
// its own (mbx.hello?, ACL pushes) reach SerialPiBackend the same way. A request the daemon rejects as malformed gets
// no reply, exactly as on the serial line. The Pi clock follows the simulation: `@MBX TIME` carries clock() before
// every request.
class SubprocessBackend : public rdm::MailboxBackend {
public:
  using ClockFn = std::function<uint32_t()>;

  struct Owner {
    uint8_t pub[32];
    uint8_t k_owner[16];
    uint8_t ttl_days = 7, sync_days = 30, quota = 20;
  };

  // mbxd_path: the daemon binary, or a .py script run with python3 (transition only); db_path: its SQLite file
  // (created by owner-add or on start). clock: Pi unix time now.
  SubprocessBackend(std::string mbxd_path, std::string db_path, ClockFn clock);
  ~SubprocessBackend() override;

  // The daemon from $RDM_MBXD, else DEFAULT_MBXD below the working directory; "" if that file is missing. Tests
  // that need the daemon fail with missingDaemon() then.
  static std::string locateMbxd();
  static const char* DEFAULT_MBXD;
  static std::string missingDaemon();

  // `mbxd --db <db> owner-add ...` as the Pi admin would; before start(), or restart() afterwards (06 par. 3.16).
  bool ownerAdd(const Owner& o);
  bool start();                       // spawn `serve --stdio --fake-clock`; false if the daemon fails to answer
  void stop();                        // close stdin and reap the child
  bool restart() { stop(); return start(); }
  bool running() const { return _pid > 0; }

  // Radio boot: a fresh SerialPiBackend (the radio's RAM is gone) says HELLO; the daemon answers with the ACL,
  // mbx.ready and mbx.time.
  void hello();

  // MailboxBackend, answered by the SerialPiBackend behind the pipe stream
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

  // Replies received from the daemon that poll() has not handed out yet.
  size_t pendingReplies() const { return _stream.pending; }
  // Number of "< mbx.<kind> <id> <code>" lines so far with this kind and code, e.g. ("store", 0x00).
  size_t countReplies(const char* kind, uint8_t code) const;
  const std::string& lastError() const { return _error; }

private:
  // Stream over the pipes for SerialPiBackend: a written line goes to the daemon behind a clock update and is
  // followed by the marker STAT; every reply line up to the marker's is buffered for reading. Markers stay out of
  // the log.
  class PipeStream : public Stream {
  public:
    explicit PipeStream(SubprocessBackend& owner) : _owner(owner) {}

    int available() override { return (int)(_rx.size() - _rx_pos); }
    int read() override { return _rx_pos < _rx.size() ? (uint8_t)_rx[_rx_pos++] : -1; }
    int peek() override { return _rx_pos < _rx.size() ? (uint8_t)_rx[_rx_pos] : -1; }
    size_t write(uint8_t b) override { return write(&b, 1); }
    size_t write(const uint8_t* buf, size_t len) override;
    using Print::write;

    // Sends `line` (empty: only the clock and the marker) and reads the replies; false on a pipe or timeout error.
    bool exchange(const std::string& line);
    void reset();                     // radio boot: unread bytes and the pending count are gone with the RAM
    void daemonStarted();             // a fresh daemon: nothing in the pipe, its fake clock at 0

    size_t pending = 0;
    std::vector<std::string> log;     // every line in both directions: "> " radio to Pi, "< " Pi to radio

  private:
    SubprocessBackend& _owner;
    std::string _tx;                  // bytes written but not yet a whole line
    std::string _rx;                  // reply lines for SerialPiBackend
    size_t _rx_pos = 0;
    std::string _pipe_rx;             // bytes read from the pipe but not yet split into lines
    uint32_t _marker = 0xF0000000u;   // ids above MailboxCore's counter range
    uint32_t _time_sent = 0;          // last `@MBX TIME` value; a fresh daemon starts its fake clock at 0

    bool writeLine(const std::string& line);
    bool readLine(std::string& line, int timeout_ms);
  };

  // SerialPiBackend keeps time in milliseconds; the Pi clock of the simulation counts seconds.
  class Clock : public mesh::MillisecondClock {
    ClockFn& _clock;
  public:
    explicit Clock(ClockFn& clock) : _clock(clock) {}
    unsigned long getMillis() override { return (unsigned long)_clock() * 1000UL; }
  };

  std::string _mbxd, _db;
  ClockFn _clock;
  Clock _ms;
  PipeStream _stream;
  std::unique_ptr<SerialPiBackend> _pi;
  pid_t _pid = -1;
  int _to_child = -1, _from_child = -1;
  std::string _error;

  bool fail(const std::string& why);
  // Every reply is in the stream once the request returned; parsing it now keeps the moment mbx.time arrived exact.
  bool after(bool sent);
};
