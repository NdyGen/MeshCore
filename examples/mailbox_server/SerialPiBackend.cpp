#include "SerialPiBackend.h"

#include <stdio.h>
#include <string.h>

using namespace rdm;

namespace {

const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

char* putHex(char* p, const uint8_t* b, size_t n) {
  static const char digits[] = "0123456789abcdef";
  for (size_t i = 0; i < n; i++) {
    *p++ = digits[b[i] >> 4];
    *p++ = digits[b[i] & 15];
  }
  return p;
}

char* putB64(char* p, const uint8_t* b, size_t n) {
  for (size_t i = 0; i < n; i += 3) {
    uint32_t v = (uint32_t)b[i] << 16;
    if (i + 1 < n) v |= (uint32_t)b[i + 1] << 8;
    if (i + 2 < n) v |= b[i + 2];
    *p++ = B64[(v >> 18) & 63];
    *p++ = B64[(v >> 12) & 63];
    *p++ = i + 1 < n ? B64[(v >> 6) & 63] : '=';
    *p++ = i + 2 < n ? B64[v & 63] : '=';
  }
  return p;
}

int b64Value(char c) {
  const char* p = c ? strchr(B64, c) : nullptr;
  return p ? (int)(p - B64) : -1;
}

// Strict: padding required, no stray characters. A corrupted copy must not reach Alice, who would report it
// UNDECRYPTABLE and so reject a good message.
bool parseB64(const char* s, uint8_t* out, size_t max, uint8_t& len) {
  size_t n = strlen(s);
  if (n == 0 || n % 4) return false;
  size_t o = 0;
  for (size_t i = 0; i < n; i += 4) {
    int v[4];
    int pad = 0;
    for (int k = 0; k < 4; k++) {
      char c = s[i + k];
      if (c == '=' && i + 4 == n && k >= 2) {
        v[k] = 0;
        pad++;
      } else if (pad || (v[k] = b64Value(c)) < 0) {
        return false;
      }
    }
    uint32_t w = ((uint32_t)v[0] << 18) | ((uint32_t)v[1] << 12) | ((uint32_t)v[2] << 6) | (uint32_t)v[3];
    uint8_t bytes[3] = {(uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w};
    for (int k = 0; k < 3 - pad; k++) {
      if (o == max) return false;
      out[o++] = bytes[k];
    }
  }
  len = (uint8_t)o;
  return true;
}

int hexValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool parseHex(const char* s, uint8_t* out, size_t n) {
  if (strlen(s) != 2 * n) return false;
  for (size_t i = 0; i < n; i++) {
    int hi = hexValue(s[2 * i]), lo = hexValue(s[2 * i + 1]);
    if (hi < 0 || lo < 0) return false;
    out[i] = (uint8_t)(hi << 4 | lo);
  }
  return true;
}

bool parseU32(const char* s, uint32_t& out) {
  size_t n = strlen(s);
  if (n == 0 || n > 10) return false;
  uint64_t v = 0;
  for (size_t i = 0; i < n; i++) {
    if (s[i] < '0' || s[i] > '9') return false;
    v = v * 10 + (uint64_t)(s[i] - '0');
  }
  if (v > 0xFFFFFFFFull) return false;
  out = (uint32_t)v;
  return true;
}

bool parseU8(const char* s, uint8_t& out) {
  uint32_t v;
  if (!parseU32(s, v) || v > 255) return false;
  out = (uint8_t)v;
  return true;
}

// Splits on single spaces in place; an empty field (double space) makes the line invalid.
int split(char* s, char** fields, int max) {
  int n = 0;
  for (;;) {
    if (n == max) return -1;
    fields[n++] = s;
    char* sp = strchr(s, ' ');
    if (!sp) break;
    *sp = 0;
    s = sp + 1;
  }
  for (int i = 0; i < n; i++) if (!*fields[i]) return -1;
  return n;
}

}

SerialPiBackend::SerialPiBackend(Stream& io, mesh::MillisecondClock& ms) : _io(io), _ms(ms) {}

void SerialPiBackend::begin(const char* fw_version) {
  _fw = fw_version;
  _ready = false;
  _daemon_proto = 0;
  sendHello();
}

void SerialPiBackend::sendHello() {
  char line[64];
  int n = snprintf(line, sizeof(line), "@MBX HELLO %lu %s\n", (unsigned long)PROTO, _fw);
  if (n > 0 && n < (int)sizeof(line)) writeLine(line, (size_t)n);
  _hello_pending = true;
  _hello_ms = (uint32_t)_ms.getMillis();
}

void SerialPiBackend::writeLine(const char* line, size_t len) { _io.write((const uint8_t*)line, len); }

bool SerialPiBackend::store(uint32_t id, const uint8_t owner[4], const uint8_t sender_pub[32], const uint8_t pkt_hash[8],
                            const uint8_t* payload, uint8_t len) {
  if (len == 0 || len > MAX_INNER_PAYLOAD) return false;
  char line[MAX_LINE];
  char* p = line + snprintf(line, sizeof(line), "@MBX STORE %lu ", (unsigned long)id);
  p = putHex(p, owner, 4);
  *p++ = ' ';
  p = putHex(p, sender_pub, 32);
  *p++ = ' ';
  p = putHex(p, pkt_hash, 8);
  *p++ = ' ';
  p = putB64(p, payload, len);
  *p++ = '\n';
  writeLine(line, (size_t)(p - line));
  return true;
}

bool SerialPiBackend::reg(uint32_t id, const uint8_t sender_pub[32], const uint8_t owner[4], const uint8_t token[8]) {
  char line[MAX_LINE];
  char* p = line + snprintf(line, sizeof(line), "@MBX REG %lu ", (unsigned long)id);
  p = putHex(p, sender_pub, 32);
  *p++ = ' ';
  p = putHex(p, owner, 4);
  *p++ = ' ';
  p = putHex(p, token, 8);
  *p++ = '\n';
  writeLine(line, (size_t)(p - line));
  return true;
}

bool SerialPiBackend::fetch(uint32_t id, const uint8_t client_pub[32], uint8_t flags, uint32_t store_id,
                            const Report* r, uint8_t n) {
  if (n > MAX_BATCH) return false;
  char line[MAX_LINE];
  char* p = line + snprintf(line, sizeof(line), "@MBX FETCH %lu ", (unsigned long)id);
  p = putHex(p, client_pub, 32);
  p += snprintf(p, 32, " %02x %08lx ", flags, (unsigned long)store_id);
  if (n == 0) *p++ = '-';
  for (uint8_t i = 0; i < n; i++) {
    if (i) *p++ = ',';
    p = putHex(p, r[i].pkt_hash, 8);
    p += snprintf(p, 8, ":%u:", (unsigned)r[i].result);
    p = putHex(p, r[i].ack, 6);
  }
  *p++ = '\n';
  writeLine(line, (size_t)(p - line));
  return true;
}

bool SerialPiBackend::stat(uint32_t id, const uint8_t sender_pub[32], const uint8_t (*hashes)[8], uint8_t n) {
  if (n == 0 || n > MAX_BATCH) return false;
  char line[MAX_LINE];
  char* p = line + snprintf(line, sizeof(line), "@MBX STAT %lu ", (unsigned long)id);
  p = putHex(p, sender_pub, 32);
  *p++ = ' ';
  for (uint8_t i = 0; i < n; i++) {
    if (i) *p++ = ',';
    p = putHex(p, hashes[i], 8);
  }
  *p++ = '\n';
  writeLine(line, (size_t)(p - line));
  return true;
}

void SerialPiBackend::pump() {
  if (_hello_pending && (uint32_t)((uint32_t)_ms.getMillis() - _hello_ms) >= HELLO_RETRY_MS) sendHello();
  while (_io.available() > 0) {
    // Full queues: leave the rest in the UART buffer until the core has taken what is here.
    if (_reply_count == REPLIES || _acl_count == ACLS) return;
    int c = _io.read();
    if (c < 0) return;
    if (c == '\n') {
      if (!_discarding) {
        _line[_line_len] = 0;
        handleLine(_line);
      }
      _line_len = 0;
      _discarding = false;
    } else if (!_discarding) {
      if (_line_len == MAX_LINE) {
        _discarding = true;
      } else {
        _line[_line_len++] = (char)c;
      }
    }
  }
}

void SerialPiBackend::handleLine(char* line) {
  size_t n = strlen(line);
  if (n && line[n - 1] == '\r') line[--n] = 0;
  if (strncmp(line, "mbx.", 4) != 0) return;
  char* rest = line + 4;
  if (strcmp(rest, "hello?") == 0) {
    sendHello();
    return;
  }
  if (strncmp(rest, "ready", 5) == 0) {
    uint32_t proto = 1;   // v1 daemons send a bare mbx.ready
    if (rest[5] != 0 && (rest[5] != ' ' || !parseU32(rest + 6, proto))) return;
    _daemon_proto = proto;
    _hello_pending = false;
    _ready = proto == PROTO;
    return;
  }
  if (strncmp(rest, "time ", 5) == 0) {
    uint32_t t;
    if (parseU32(rest + 5, t) && t != 0) {
      _time = t;
      _time_ms = (uint32_t)_ms.getMillis();
      _has_time = true;
    }
    return;
  }
  if (strncmp(rest, "acl ", 4) == 0) {
    parseAcl(rest + 4);
    return;
  }
  BackendReply& r = _replies[(_reply_head + _reply_count) % REPLIES];
  if (parseReply(rest, r)) _reply_count++;
}

bool SerialPiBackend::parseAcl(char* args) {
  char* f[4];
  if (split(args, f, 4) != 3 || strlen(f[1]) != 1 || (f[1][0] != 'o' && f[1][0] != 'd')) return false;
  Acl& a = _acls[(_acl_head + _acl_count) % ACLS];
  if (!parseHex(f[0], a.pub, 32) || !parseHex(f[2], a.owner, 4)) return false;
  a.is_owner = f[1][0] == 'o';
  _acl_count++;
  return true;
}

bool SerialPiBackend::parseReply(char* line, BackendReply& r) {
  char* sp = strchr(line, ' ');
  if (!sp) return false;
  *sp = 0;
  const char* kind = line;
  char* f[8];
  int n = split(sp + 1, f, 8);
  uint8_t code;
  memset(&r, 0, sizeof(r));
  if (n < 2 || !parseU32(f[0], r.id) || !parseHex(f[1], &code, 1)) return false;
  r.code = (MbxCode)code;

  if (strcmp(kind, "store") == 0) {
    r.kind = BackendReply::Kind::STORE;
    return n == 3 && parseU32(f[2], r.expires);
  }
  if (strcmp(kind, "reg") == 0) {
    r.kind = BackendReply::Kind::REG;
    return n == 4 && parseU8(f[2], r.ttl_days) && parseU8(f[3], r.quota);
  }
  if (strcmp(kind, "fetch") == 0) {
    r.kind = BackendReply::Kind::FETCH;
    if (n != 5 || !parseU8(f[2], r.remaining) || !parseU8(f[3], r.reports_ok)) return false;
    if (strcmp(f[4], "-") == 0) return true;
    return parseB64(f[4], r.inner, MAX_INNER_PAYLOAD, r.inner_len);
  }
  if (strcmp(kind, "stat") == 0) {
    r.kind = BackendReply::Kind::STAT;
    if (n != 3) return false;
    if (strcmp(f[2], "-") == 0) return true;
    char* item = f[2];
    for (;;) {
      char* comma = strchr(item, ',');
      if (comma) *comma = 0;
      char* colon = strchr(item, ':');
      uint8_t state;
      if (r.n == MAX_BATCH || !colon) return false;
      *colon = 0;
      if (!parseU8(item, state) || state > (uint8_t)MbxState::SYNC_EXPIRED || !parseHex(colon + 1, r.items[r.n].ack, 6))
        return false;
      r.items[r.n++].state = (MbxState)state;
      if (!comma) return true;
      item = comma + 1;
    }
  }
  return false;
}

bool SerialPiBackend::poll(BackendReply& out) {
  pump();
  if (_reply_count == 0) return false;
  out = _replies[_reply_head];
  _reply_head = (_reply_head + 1) % REPLIES;
  _reply_count--;
  return true;
}

bool SerialPiBackend::pollAcl(uint8_t pub_out[32], bool& is_owner, uint8_t owner_out[4]) {
  pump();
  if (_acl_count == 0) return false;
  const Acl& a = _acls[_acl_head];
  memcpy(pub_out, a.pub, 32);
  is_owner = a.is_owner;
  memcpy(owner_out, a.owner, 4);
  _acl_head = (_acl_head + 1) % ACLS;
  _acl_count--;
  return true;
}

bool SerialPiBackend::ready() {
  pump();
  return _ready;
}

uint32_t SerialPiBackend::daemonProto() {
  pump();
  return _daemon_proto;
}

uint32_t SerialPiBackend::backendTime() {
  pump();
  if (!_has_time) return 0;
  return _time + ((uint32_t)_ms.getMillis() - _time_ms) / 1000;
}
