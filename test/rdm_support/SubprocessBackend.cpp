#include "SubprocessBackend.h"

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

const int REPLY_TIMEOUT_MS = 10000;   // real time; mbxd answers in milliseconds, this only guards a hung child

std::string hex(const uint8_t* p, size_t n) {
  static const char* digits = "0123456789abcdef";
  std::string s;
  for (size_t i = 0; i < n; i++) {
    s += digits[p[i] >> 4];
    s += digits[p[i] & 15];
  }
  return s;
}

bool unhex(const std::string& s, uint8_t* out, size_t n) {
  if (s.size() != n * 2) return false;
  for (size_t i = 0; i < n; i++) {
    unsigned v;
    if (sscanf(s.c_str() + 2 * i, "%2x", &v) != 1) return false;
    out[i] = (uint8_t)v;
  }
  return true;
}

const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string b64(const uint8_t* p, size_t n) {
  std::string s;
  for (size_t i = 0; i < n; i += 3) {
    uint32_t v = (uint32_t)p[i] << 16 | (i + 1 < n ? (uint32_t)p[i + 1] << 8 : 0) | (i + 2 < n ? p[i + 2] : 0);
    s += B64[v >> 18 & 63];
    s += B64[v >> 12 & 63];
    s += i + 1 < n ? B64[v >> 6 & 63] : '=';
    s += i + 2 < n ? B64[v & 63] : '=';
  }
  return s;
}

bool unb64(const std::string& s, std::vector<uint8_t>& out) {
  out.clear();
  if (s.size() % 4) return false;
  uint32_t v = 0;
  int bits = 0;
  for (char c : s) {
    if (c == '=') break;
    const char* at = strchr(B64, c);
    if (!at || !c) return false;
    v = v << 6 | (uint32_t)(at - B64);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back((uint8_t)(v >> bits));
    }
  }
  return true;
}

std::vector<std::string> split(const std::string& s, char sep) {
  std::vector<std::string> parts;
  size_t start = 0;
  for (;;) {
    size_t at = s.find(sep, start);
    parts.push_back(s.substr(start, at - start));
    if (at == std::string::npos) return parts;
    start = at + 1;
  }
}

bool isFile(const std::string& path) {
  struct stat st;
  return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

// fork/exec with optional pipes; stderr goes to /dev/null unless SIM_VERBOSE is set (mbxd logs every request).
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

}  // namespace

SubprocessBackend::SubprocessBackend(std::string mbxd_path, std::string db_path, ClockFn clock)
    : _mbxd(std::move(mbxd_path)), _db(std::move(db_path)), _clock(std::move(clock)) {}

SubprocessBackend::~SubprocessBackend() { stop(); }

std::string SubprocessBackend::locateMbxd() {
  const char* env = getenv("RDM_MBXD");
  if (env && *env) return isFile(env) ? env : "";
  const char* rel = "examples/mailbox_server/mbxd/mbxd.py";
  return isFile(rel) ? rel : "";
}

bool SubprocessBackend::ownerAdd(const Owner& o) {
  std::vector<std::string> argv = { "python3", _mbxd, "--db", _db, "owner-add", hex(o.pub, 32),
                                    "--k-owner", hex(o.k_owner, 16), "--ttl-days", std::to_string(o.ttl_days),
                                    "--sync-days", std::to_string(o.sync_days), "--quota", std::to_string(o.quota) };
  pid_t pid = spawn(argv, nullptr, nullptr);
  if (pid < 0) return fail("owner-add: spawn failed");
  int status = 0;
  if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    return fail("owner-add: mbxd exited with an error");
  }
  return true;
}

bool SubprocessBackend::start() {
  if (_pid > 0) return true;
  signal(SIGPIPE, SIG_IGN);   // a dead child must show up as a failed write, not kill the test
  _pid = spawn({ "python3", _mbxd, "--db", _db, "serve", "--stdio", "--fake-clock" }, &_to_child, &_from_child);
  if (_pid < 0) return fail("serve: spawn failed");
  _rx.clear();
  _ready = false;
  _has_time = false;
  _time_sent = 0;
  // the fake clock starts at 0: set it first, and prove the child answers
  return request("");
}

void SubprocessBackend::stop() {
  if (_to_child >= 0) close(_to_child);
  if (_from_child >= 0) close(_from_child);
  _to_child = _from_child = -1;
  if (_pid > 0) waitpid(_pid, nullptr, 0);
  _pid = -1;
  _ready = false;
}

void SubprocessBackend::hello() {
  _ready = false;
  _acl.clear();
  request("@MBX HELLO sim");
}

bool SubprocessBackend::fail(const std::string& why) {
  _error = why;
  return false;
}

bool SubprocessBackend::writeLine(const std::string& line) {
  std::string buf = line + "\n";
  size_t off = 0;
  while (off < buf.size()) {
    ssize_t n = write(_to_child, buf.data() + off, buf.size() - off);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return fail("write to mbxd failed");
    off += (size_t)n;
  }
  return true;
}

bool SubprocessBackend::readLine(std::string& line, int timeout_ms) {
  for (;;) {
    size_t nl = _rx.find('\n');
    if (nl != std::string::npos) {
      line = _rx.substr(0, nl);
      _rx.erase(0, nl + 1);
      return true;
    }
    struct pollfd p = { _from_child, POLLIN, 0 };
    int r = ::poll(&p, 1, timeout_ms);
    if (r < 0 && errno == EINTR) continue;
    if (r <= 0) return fail("no line from mbxd within the timeout");
    char buf[4096];
    ssize_t n = read(_from_child, buf, sizeof(buf));
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return fail("mbxd closed its output");
    _rx.append(buf, (size_t)n);
  }
}

// Writes `line` (if any) behind a clock update and reads every reply up to the marker's.
bool SubprocessBackend::request(const std::string& line) {
  if (_pid <= 0) return fail("mbxd not running");
  uint32_t now = _clock();
  if (now != _time_sent) {
    if (!writeLine("@MBX TIME " + std::to_string(now))) return false;
    _time_sent = now;
  }
  if (!line.empty()) {
    _log.push_back("> " + line);
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
    handleLine(reply);
  }
  return false;
}

void SubprocessBackend::handleLine(const std::string& line) {
  _log.push_back("< " + line);
  std::vector<std::string> f = split(line, ' ');
  const std::string& kind = f[0];
  if (kind == "mbx.ready") {
    _ready = true;
    return;
  }
  if (kind == "mbx.time" && f.size() == 2) {
    _mbx_time = (uint32_t)strtoul(f[1].c_str(), nullptr, 10);
    _mbx_time_at = _clock();
    _has_time = true;
    return;
  }
  if (kind == "mbx.acl" && f.size() == 4) {
    Acl a;
    if (unhex(f[1], a.pub, 32) && unhex(f[3], a.owner, 4) && (f[2] == "o" || f[2] == "d")) {
      a.is_owner = f[2] == "o";
      _acl.push_back(a);
    }
    return;
  }
  if (f.size() < 3) return;

  rdm::BackendReply r;
  memset(&r, 0, sizeof(r));
  r.id = (uint32_t)strtoul(f[1].c_str(), nullptr, 10);
  r.code = (rdm::MbxCode)strtoul(f[2].c_str(), nullptr, 16);
  if (kind == "mbx.store" && f.size() == 4) {
    r.kind = rdm::BackendReply::Kind::STORE;
    r.expires = (uint32_t)strtoul(f[3].c_str(), nullptr, 10);
  } else if (kind == "mbx.reg" && f.size() == 5) {
    r.kind = rdm::BackendReply::Kind::REG;
    r.ttl_days = (uint8_t)atoi(f[3].c_str());
    r.quota = (uint8_t)atoi(f[4].c_str());
  } else if (kind == "mbx.fetch" && f.size() == 6) {
    r.kind = rdm::BackendReply::Kind::FETCH;
    r.remaining = (uint8_t)atoi(f[3].c_str());
    r.reports_ok = (uint8_t)atoi(f[4].c_str());
    if (f[5] != "-") {
      std::vector<uint8_t> inner;
      if (!unb64(f[5], inner) || inner.size() > sizeof(r.inner)) return;
      memcpy(r.inner, inner.data(), inner.size());
      r.inner_len = (uint8_t)inner.size();
    }
  } else if (kind == "mbx.stat" && f.size() == 4) {
    r.kind = rdm::BackendReply::Kind::STAT;
    if (f[3] != "-") {
      for (const std::string& item : split(f[3], ',')) {
        std::vector<std::string> sa = split(item, ':');
        if (sa.size() != 2 || r.n >= rdm::MAX_BATCH || !unhex(sa[1], r.items[r.n].ack, 6)) return;
        r.items[r.n].state = (rdm::MbxState)atoi(sa[0].c_str());
        r.n++;
      }
    }
  } else {
    return;
  }
  _replies.push_back(r);
}

bool SubprocessBackend::store(uint32_t id, const uint8_t owner[4], const uint8_t sender_pub[32],
                              const uint8_t pkt_hash[8], const uint8_t* payload, uint8_t len) {
  return request("@MBX STORE " + std::to_string(id) + " " + hex(owner, 4) + " " + hex(sender_pub, 32) + " " +
                 hex(pkt_hash, 8) + " " + b64(payload, len));
}

bool SubprocessBackend::reg(uint32_t id, const uint8_t sender_pub[32], const uint8_t owner[4], const uint8_t token[8]) {
  return request("@MBX REG " + std::to_string(id) + " " + hex(sender_pub, 32) + " " + hex(owner, 4) + " " +
                 hex(token, 8));
}

bool SubprocessBackend::fetch(uint32_t id, const uint8_t client_pub[32], uint8_t flags, uint32_t store_id,
                              const rdm::Report* r, uint8_t n) {
  char head[16];
  snprintf(head, sizeof(head), "%02x %08x", flags, (unsigned)store_id);
  std::string reports;
  for (uint8_t i = 0; i < n; i++) {
    if (i) reports += ",";
    reports += hex(r[i].pkt_hash, 8) + ":" + std::to_string((unsigned)r[i].result) + ":" + hex(r[i].ack, 6);
  }
  return request("@MBX FETCH " + std::to_string(id) + " " + hex(client_pub, 32) + " " + head + " " +
                 (n ? reports : "-"));
}

bool SubprocessBackend::stat(uint32_t id, const uint8_t sender_pub[32], const uint8_t (*hashes)[8], uint8_t n) {
  std::string list;
  for (uint8_t i = 0; i < n; i++) {
    if (i) list += ",";
    list += hex(hashes[i], 8);
  }
  return request("@MBX STAT " + std::to_string(id) + " " + hex(sender_pub, 32) + " " + list);
}

bool SubprocessBackend::poll(rdm::BackendReply& out) {
  if (_replies.empty()) return false;
  out = _replies.front();
  _replies.pop_front();
  return true;
}

bool SubprocessBackend::pollAcl(uint8_t pub_out[32], bool& is_owner, uint8_t owner_out[4]) {
  if (_acl.empty()) return false;
  const Acl& a = _acl.front();
  memcpy(pub_out, a.pub, 32);
  is_owner = a.is_owner;
  memcpy(owner_out, a.owner, 4);
  _acl.pop_front();
  return true;
}

uint32_t SubprocessBackend::backendTime() {
  if (!_has_time) return 0;
  return _mbx_time + (_clock() - _mbx_time_at);
}

size_t SubprocessBackend::countReplies(const char* kind, uint8_t code) const {
  char prefix[32];
  snprintf(prefix, sizeof(prefix), "< mbx.%s ", kind);
  char code_s[8];
  snprintf(code_s, sizeof(code_s), " %02x", code);
  size_t n = 0;
  for (const auto& l : _log) {
    if (l.compare(0, strlen(prefix), prefix) != 0) continue;
    std::vector<std::string> f = split(l.substr(2), ' ');
    if (f.size() >= 3 && " " + f[2] == code_s) n++;
  }
  return n;
}
