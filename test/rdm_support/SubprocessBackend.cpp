#include "SubprocessBackend.h"

#include <TestUtil.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

const int REPLY_TIMEOUT_MS = 10000;   // real time; the daemon answers in milliseconds, this only guards a hung child

bool isFile(const std::string& path) {
  struct stat st;
  return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

// fork/exec with optional pipes; stderr goes to /dev/null unless SIM_VERBOSE is set (the daemon logs every request).
pid_t spawn(const std::vector<std::string>& argv, int* to_child, int* from_child) {
  int in[2] = { -1, -1 }, out[2] = { -1, -1 };
  if (to_child && pipe(in) != 0) return -1;
  if (from_child && pipe(out) != 0) return -1;
  pid_t pid = fork();
  if (pid < 0) return -1;
  if (pid == 0) {
    if (to_child) { dup2(in[0], 0); close(in[0]); close(in[1]); }
    if (from_child) { dup2(out[1], 1); close(out[0]); close(out[1]); }
    if (!getenv("SIM_VERBOSE")) {
      int null_fd = open("/dev/null", O_WRONLY);
      if (null_fd >= 0) { dup2(null_fd, 2); if (!from_child) dup2(null_fd, 1); }
    }
    std::vector<char*> args;
    for (const auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);
    execvp(args[0], args.data());
    _exit(127);
  }
  if (to_child) { close(in[0]); *to_child = in[1]; }
  if (from_child) { close(out[1]); *from_child = out[0]; }
  return pid;
}

// A .py path runs under python3 (transition while the Python daemon is still in the tree); anything else is executed directly.
std::vector<std::string> daemonArgv(const std::string& mbxd, std::vector<std::string> args) {
  std::vector<std::string> argv;
  if (mbxd.size() > 3 && mbxd.compare(mbxd.size() - 3, 3, ".py") == 0) argv.push_back("python3");
  argv.push_back(mbxd);
  argv.insert(argv.end(), args.begin(), args.end());
  return argv;
}

bool startsWith(const std::string& s, const char* prefix) { return s.compare(0, strlen(prefix), prefix) == 0; }

// A reply to a request (never the session lines ready/time/acl/hello?), so poll() will hand it out.
bool isRequestReply(const std::string& line) {
  return startsWith(line, "mbx.store ") || startsWith(line, "mbx.reg ") || startsWith(line, "mbx.fetch ") ||
         startsWith(line, "mbx.stat ");
}

}  // namespace

const char* SubprocessBackend::DEFAULT_MBXD = "examples/mailbox_server/meshcore-mailboxd/target/release/meshcore-mailboxd";

SubprocessBackend::SubprocessBackend(std::string mbxd_path, std::string db_path, ClockFn clock)
    : _mbxd(std::move(mbxd_path)), _db(std::move(db_path)), _clock(std::move(clock)), _ms(_clock), _stream(*this) {}

SubprocessBackend::~SubprocessBackend() { stop(); }

std::string SubprocessBackend::locateMbxd() {
  const char* env = getenv("RDM_MBXD");
  if (env && *env) return isFile(env) ? env : "";
  return isFile(DEFAULT_MBXD) ? DEFAULT_MBXD : "";
}

std::string SubprocessBackend::missingDaemon() {
  return std::string("meshcore-mailboxd not found: run `cargo build --release` in examples/mailbox_server/meshcore-mailboxd "
                     "(expected ") + DEFAULT_MBXD + " below the working directory, or set RDM_MBXD to the binary)";
}

bool SubprocessBackend::fail(const std::string& why) {
  _error = why;
  return false;
}

bool SubprocessBackend::ownerAdd(const Owner& o) {
  if (_mbxd.empty()) return fail(missingDaemon());
  std::vector<std::string> argv = daemonArgv(_mbxd, { "--db", _db, "owner-add", toHexLower(o.pub, 32),
                                                      "--k-owner", toHexLower(o.k_owner, 16), "--ttl-days", std::to_string(o.ttl_days),
                                                      "--sync-days", std::to_string(o.sync_days), "--quota", std::to_string(o.quota) });
  pid_t pid = spawn(argv, nullptr, nullptr);
  if (pid < 0) return fail("owner-add: spawn failed");
  int status = 0;
  if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    return fail("owner-add: the daemon exited with an error");
  }
  return true;
}

bool SubprocessBackend::start() {
  if (_pid > 0) return true;
  if (_mbxd.empty()) return fail(missingDaemon());
  signal(SIGPIPE, SIG_IGN);   // a dead child must show up as a failed write, not kill the test
  _pid = spawn(daemonArgv(_mbxd, { "--db", _db, "serve", "--stdio", "--fake-clock" }), &_to_child, &_from_child);
  if (_pid < 0) return fail("serve: spawn failed");
  _stream.daemonStarted();
  _pi.reset(new SerialPiBackend(_stream, _ms));
  // the fake clock starts at 0: set it first, and prove the child answers
  return _stream.exchange("");
}

void SubprocessBackend::stop() {
  if (_to_child >= 0) close(_to_child);
  if (_from_child >= 0) close(_from_child);
  _to_child = _from_child = -1;
  if (_pid > 0) waitpid(_pid, nullptr, 0);
  _pid = -1;
  _pi.reset();
}

void SubprocessBackend::hello() {
  if (_pid <= 0) return;
  _error.clear();
  _stream.reset();
  _pi.reset(new SerialPiBackend(_stream, _ms));
  _pi->begin("sim");
  after(true);
}

bool SubprocessBackend::after(bool sent) {
  if (!sent || !_error.empty()) return false;
  _pi->ready();
  return true;
}

bool SubprocessBackend::store(uint32_t id, const uint8_t owner[4], const uint8_t sender_pub[32],
                              const uint8_t pkt_hash[8], const uint8_t* payload, uint8_t len) {
  if (_pid <= 0) return fail("daemon not running");
  _error.clear();
  return after(_pi->store(id, owner, sender_pub, pkt_hash, payload, len));
}

bool SubprocessBackend::reg(uint32_t id, const uint8_t sender_pub[32], const uint8_t owner[4], const uint8_t token[8]) {
  if (_pid <= 0) return fail("daemon not running");
  _error.clear();
  return after(_pi->reg(id, sender_pub, owner, token));
}

bool SubprocessBackend::fetch(uint32_t id, const uint8_t client_pub[32], uint8_t flags, uint32_t store_id,
                              const rdm::Report* r, uint8_t n) {
  if (_pid <= 0) return fail("daemon not running");
  _error.clear();
  return after(_pi->fetch(id, client_pub, flags, store_id, r, n));
}

bool SubprocessBackend::stat(uint32_t id, const uint8_t sender_pub[32], const uint8_t (*hashes)[8], uint8_t n) {
  if (_pid <= 0) return fail("daemon not running");
  _error.clear();
  return after(_pi->stat(id, sender_pub, hashes, n));
}

bool SubprocessBackend::poll(rdm::BackendReply& out) {
  if (!_pi || !_pi->poll(out)) return false;
  if (_stream.pending > 0) _stream.pending--;
  return true;
}

bool SubprocessBackend::pollAcl(uint8_t pub_out[32], bool& is_owner, uint8_t owner_out[4]) {
  return _pi && _pi->pollAcl(pub_out, is_owner, owner_out);
}

bool SubprocessBackend::ready() { return _pi && _pi->ready(); }

uint32_t SubprocessBackend::backendTime() { return _pi ? _pi->backendTime() : 0; }

size_t SubprocessBackend::countReplies(const char* kind, uint8_t code) const {
  char prefix[32];
  snprintf(prefix, sizeof(prefix), "< mbx.%s ", kind);
  char code_s[8];
  snprintf(code_s, sizeof(code_s), "%02x", code);
  size_t n = 0;
  for (const auto& l : _stream.log) {
    if (l.compare(0, strlen(prefix), prefix) != 0) continue;
    // "< mbx.kind id code ...": the code is the third field
    size_t id_at = strlen(prefix);
    size_t code_at = l.find(' ', id_at);
    if (code_at == std::string::npos) continue;
    code_at++;
    if (l.compare(code_at, 2, code_s) == 0 && (l.size() == code_at + 2 || l[code_at + 2] == ' ')) n++;
  }
  return n;
}

// ---------------------------------------------------------------- PipeStream

void SubprocessBackend::PipeStream::reset() {
  _tx.clear();
  _rx.clear();
  _rx_pos = 0;
  pending = 0;
}

void SubprocessBackend::PipeStream::daemonStarted() {
  reset();
  _pipe_rx.clear();
  _time_sent = 0;
}

size_t SubprocessBackend::PipeStream::write(const uint8_t* buf, size_t len) {
  for (size_t i = 0; i < len; i++) {
    if (buf[i] != '\n') {
      _tx += (char)buf[i];
      continue;
    }
    std::string line;
    line.swap(_tx);
    if (!exchange(line)) return i;
  }
  return len;
}

bool SubprocessBackend::PipeStream::writeLine(const std::string& line) {
  std::string buf = line + "\n";
  size_t off = 0;
  while (off < buf.size()) {
    ssize_t n = ::write(_owner._to_child, buf.data() + off, buf.size() - off);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return _owner.fail("write to the daemon failed");
    off += (size_t)n;
  }
  return true;
}

bool SubprocessBackend::PipeStream::readLine(std::string& line, int timeout_ms) {
  for (;;) {
    size_t nl = _pipe_rx.find('\n');
    if (nl != std::string::npos) {
      line = _pipe_rx.substr(0, nl);
      _pipe_rx.erase(0, nl + 1);
      return true;
    }
    struct pollfd p = { _owner._from_child, POLLIN, 0 };
    int r = ::poll(&p, 1, timeout_ms);
    if (r < 0 && errno == EINTR) continue;
    if (r <= 0) return _owner.fail("no line from the daemon within the timeout");
    char buf[4096];
    ssize_t n = ::read(_owner._from_child, buf, sizeof(buf));
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return _owner.fail("the daemon closed its output");
    _pipe_rx.append(buf, (size_t)n);
  }
}

bool SubprocessBackend::PipeStream::exchange(const std::string& line) {
  if (_owner._pid <= 0) return _owner.fail("daemon not running");
  uint32_t now = _owner._clock();
  if (now != _time_sent) {
    if (!writeLine("@MBX TIME " + std::to_string(now))) return false;
    _time_sent = now;
  }
  if (!line.empty()) {
    log.push_back("> " + line);
    if (!writeLine(line)) return false;
  }
  uint32_t marker = _marker++;
  char probe[128];
  snprintf(probe, sizeof(probe), "@MBX STAT %u %s %s", (unsigned)marker, std::string(64, '0').c_str(),
           std::string(16, '0').c_str());
  if (!writeLine(probe)) return false;
  char end[32];
  int end_len = snprintf(end, sizeof(end), "mbx.stat %u ", (unsigned)marker);
  std::string reply;
  while (readLine(reply, REPLY_TIMEOUT_MS)) {
    if (reply.compare(0, (size_t)end_len, end) == 0) return true;
    log.push_back("< " + reply);
    if (isRequestReply(reply)) pending++;
    if (_rx_pos == _rx.size()) {
      _rx.clear();
      _rx_pos = 0;
    }
    _rx += reply + "\n";
  }
  return false;
}
