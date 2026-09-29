#include "RdmScenario.h"

#include <Packet.h>
#include <Utils.h>
#include <helpers/rdm/RdmCodec.h>
#include <helpers/rdm/RdmCrypto.h>
#include <helpers/rdm/RdmStorage.h>

#include "SimFileIO.h"

#include <stdio.h>
#include <string.h>

#include <algorithm>

namespace sim {

const char* kindName(Kind k) {
  switch (k) {
    case Kind::ADVERT: return "ADVERT";
    case Kind::DM: return "DM";
    case Kind::CTRL: return "CTRL";
    case Kind::ACK: return "ACK";
    case Kind::PATH: return "PATH";
    case Kind::QUERY: return "QUERY";
    case Kind::QUERY_RESP: return "QUERY_RESP";
    case Kind::DEPOSIT: return "DEPOSIT";
    case Kind::DEPOSIT_RESP: return "DEPOSIT_RESP";
    case Kind::FETCH: return "FETCH";
    case Kind::FETCH_RESP: return "FETCH_RESP";
    case Kind::STATUS: return "STATUS";
    case Kind::STATUS_RESP: return "STATUS_RESP";
    case Kind::REG: return "REG";
    case Kind::REG_RESP: return "REG_RESP";
    case Kind::REQ_OTHER: return "REQ";
    case Kind::RESP_OTHER: return "RESPONSE";
    default: return "?";
  }
}

const char* statusName(rdm::UserStatus s) {
  switch (s) {
    case rdm::UserStatus::QUEUED: return "QUEUED";
    case rdm::UserStatus::CUSTODY: return "CUSTODY";
    case rdm::UserStatus::ON_RADIO: return "ON_RADIO";
    case rdm::UserStatus::DELIVERED: return "DELIVERED";
    case rdm::UserStatus::ON_RADIO_FINAL: return "ON_RADIO_FINAL";
    case rdm::UserStatus::REJECTED: return "REJECTED";
    case rdm::UserStatus::EXPIRED: return "EXPIRED";
    case rdm::UserStatus::SYNC_EXPIRED: return "SYNC_EXPIRED";
    case rdm::UserStatus::TOO_BIG: return "TOO_BIG";
    case rdm::UserStatus::NO_OUTBOX: return "NO_OUTBOX";
  }
  return "?";
}

std::string toString(const std::vector<rdm::UserStatus>& v) {
  std::string s = "[";
  for (size_t i = 0; i < v.size(); i++) s += (i ? ", " : "") + std::string(statusName(v[i]));
  return s + "]";
}

// ---- WireLog --------------------------------------------------------------------------------------------

void WireLog::addNode(SimNode& n) { _nodes.push_back(&n); }

int WireLog::nodeByPub(const uint8_t* pub) const {
  for (SimNode* n : _nodes) {
    if (memcmp(n->identity().pub_key, pub, PUB_KEY_SIZE) == 0) return n->index();
  }
  return -1;
}

const std::vector<uint8_t>& WireLog::secret(int a, int b) {
  auto key = std::make_pair(std::min(a, b), std::max(a, b));
  auto it = _secrets.find(key);
  if (it != _secrets.end()) return it->second;
  std::vector<uint8_t> s(PUB_KEY_SIZE);
  SimNode& na = _sim.node((size_t)a);
  SimNode& nb = _sim.node((size_t)b);
  na.identity().calcSharedSecret(s.data(), nb.identity().pub_key);
  return _secrets[key] = s;
}

bool WireLog::decryptDatagram(Wire& w, const uint8_t* payload, size_t len, std::vector<uint8_t>& plain) {
  if (len < 2 + CIPHER_MAC_SIZE + 1) return false;
  uint8_t buf[MAX_PACKET_PAYLOAD + 16];
  for (SimNode* d : _nodes) {
    if (d->identity().pub_key[0] != payload[0]) continue;
    for (SimNode* o : _nodes) {
      if (o == d || o->identity().pub_key[0] != payload[1]) continue;
      int n = mesh::Utils::MACThenDecrypt(secret(o->index(), d->index()).data(), buf, payload + 2, (int)len - 2);
      if (n <= 0) continue;
      plain.assign(buf, buf + n);
      w.dest = d->index();
      if (w.origin < 0) w.origin = o->index();
      w.decrypted = true;
      return true;
    }
  }
  return false;
}

Kind WireLog::classifyResponse(uint32_t tag, int requester, int responder) const {
  for (auto it = _reqs.rbegin(); it != _reqs.rend(); ++it) {
    if (it->tag == tag && it->requester == requester && it->responder == responder) {
      switch (it->kind) {
        case Kind::QUERY: return Kind::QUERY_RESP;
        case Kind::DEPOSIT: return Kind::DEPOSIT_RESP;
        case Kind::FETCH: return Kind::FETCH_RESP;
        case Kind::STATUS: return Kind::STATUS_RESP;
        case Kind::REG: return Kind::REG_RESP;
        default: return Kind::RESP_OTHER;
      }
    }
  }
  return Kind::RESP_OTHER;
}

static uint32_t le32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

void WireLog::classifyPlain(Wire& w, const std::vector<uint8_t>& plain) {
  switch (w.type) {
    case PAYLOAD_TYPE_TXT_MSG: {
      rdm::codec::TxtParsed t;
      w.body = plain;
      if (!rdm::codec::parseTxtPlain(plain.data(), plain.size(), t)) return;
      w.ts = t.ts;
      w.txt_type = t.txt_type;
      w.kind = t.txt_type == rdm::TXT_TYPE_RDM_CTRL ? Kind::CTRL : Kind::DM;
      w.text.assign(t.text, t.text_len);
      w.ext_attempt = t.has_ext_attempt;
      w.attempt = t.has_ext_attempt ? t.ext_attempt : (uint8_t)(t.flags & 3);
      w.cap = t.cap;
      return;
    }
    case PAYLOAD_TYPE_REQ: {
      if (plain.size() < 5) return;
      w.ts = le32(plain.data());
      w.body.assign(plain.begin() + 4, plain.end());
      switch (plain[4]) {
        case rdm::REQ_DEPOSIT: w.kind = Kind::DEPOSIT; break;
        case rdm::REQ_FETCH: w.kind = Kind::FETCH; break;
        case rdm::REQ_STATUS: w.kind = Kind::STATUS; break;
        case rdm::REQ_RECEIPT_QUERY: w.kind = Kind::QUERY; break;
        default: w.kind = Kind::REQ_OTHER; break;
      }
      _reqs.push_back({ w.ts, w.origin, w.dest, w.kind });
      return;
    }
    case PAYLOAD_TYPE_RESPONSE: {
      if (plain.size() < 4) return;
      w.ts = le32(plain.data());
      w.body.assign(plain.begin() + 4, plain.end());
      w.kind = classifyResponse(w.ts, w.dest, w.origin);
      return;
    }
    case PAYLOAD_TYPE_PATH: {
      w.kind = Kind::PATH;
      if (plain.empty()) return;
      uint8_t path_len = plain[0];
      size_t k = 1 + (size_t)(path_len & 63) * ((path_len >> 6) + 1);
      if (k >= plain.size()) return;
      uint8_t extra_type = plain[k++];
      if (extra_type == PAYLOAD_TYPE_ACK) {
        w.inner = Kind::ACK;
        w.ack.assign(plain.begin() + k, plain.begin() + std::min(plain.size(), k + 7));
      } else if (extra_type == PAYLOAD_TYPE_RESPONSE && plain.size() >= k + 4) {
        w.ts = le32(&plain[k]);
        w.body.assign(plain.begin() + k + 4, plain.end());
        w.inner = classifyResponse(w.ts, w.dest, w.origin);
      }
      return;
    }
  }
}

Wire WireLog::decode(const TxRecord& tx) {
  Wire w;
  w.seq = tx.seq;
  w.t_ms = tx.t_ms;
  w.airtime_ms = tx.airtime_ms;
  w.tx_node = tx.from;
  w.type = tx.payload_type;
  w.flood = tx.flood;

  mesh::Packet p;
  if (!p.readFrom(tx.raw.data(), (uint8_t)tx.raw.size())) return w;
  w.hops = p.getPathHashCount();
  p.calculatePacketHash(w.pkt_hash);
  std::string h((const char*)w.pkt_hash, sizeof(w.pkt_hash));
  auto first = _first_tx.find(h);
  if (first == _first_tx.end()) first = _first_tx.emplace(h, tx.from).first;
  w.origin = first->second;

  std::vector<uint8_t> plain;
  switch (w.type) {
    case PAYLOAD_TYPE_ACK:
      w.kind = Kind::ACK;
      w.ack.assign(p.payload, p.payload + p.payload_len);
      break;
    case PAYLOAD_TYPE_ADVERT:
      w.kind = Kind::ADVERT;
      if (p.payload_len >= PUB_KEY_SIZE) {
        int o = nodeByPub(p.payload);
        if (o >= 0) w.origin = o;
      }
      break;
    case PAYLOAD_TYPE_ANON_REQ: {
      if (p.payload_len < 1 + PUB_KEY_SIZE + CIPHER_MAC_SIZE + 1) break;
      int o = nodeByPub(&p.payload[1]);
      if (o >= 0) w.origin = o;
      uint8_t buf[MAX_PACKET_PAYLOAD + 16];
      for (SimNode* d : _nodes) {
        if (d->identity().pub_key[0] != p.payload[0] || d->index() == o) continue;
        uint8_t s[PUB_KEY_SIZE];
        d->identity().calcSharedSecret(s, &p.payload[1]);
        int n = mesh::Utils::MACThenDecrypt(s, buf, &p.payload[1 + PUB_KEY_SIZE], p.payload_len - 1 - PUB_KEY_SIZE);
        if (n <= 0) continue;
        w.decrypted = true;
        w.dest = d->index();
        w.body.assign(buf, buf + n);
        if (n >= 6) {
          w.ts = le32(buf);
          w.kind = buf[4] == rdm::REG_MARKER0 && buf[5] == rdm::REG_MARKER1 ? Kind::REG : Kind::REQ_OTHER;
          _reqs.push_back({ w.ts, w.origin, w.dest, w.kind });
        }
        break;
      }
      break;
    }
    case PAYLOAD_TYPE_TXT_MSG:
    case PAYLOAD_TYPE_REQ:
    case PAYLOAD_TYPE_RESPONSE:
    case PAYLOAD_TYPE_PATH:
      if (decryptDatagram(w, p.payload, p.payload_len, plain)) classifyPlain(w, plain);
      break;
  }
  return w;
}

const std::vector<Wire>& WireLog::all() {
  const auto& log = _sim.tx_log();
  for (; _decoded < log.size(); _decoded++) _wires.push_back(decode(log[_decoded]));
  return _wires;
}

std::vector<Wire> WireLog::select(const std::function<bool(const Wire&)>& pred) {
  std::vector<Wire> out;
  for (const Wire& w : all()) {
    if (pred(w)) out.push_back(w);
  }
  return out;
}

std::vector<Wire> WireLog::sent(const SimNode& origin, Kind k, uint64_t from_ms, uint64_t to_ms) {
  int o = origin.index();
  return select([&](const Wire& w) {
    return w.original() && w.origin == o && w.effective() == k && w.t_ms >= from_ms && w.t_ms < to_ms;
  });
}

uint64_t WireLog::airtime(const std::function<bool(const Wire&)>& pred) {
  uint64_t sum = 0;
  for (const Wire& w : all()) {
    if (pred(w)) sum += w.airtime_ms;
  }
  return sum;
}

std::string WireLog::describe(const Wire& w) const {
  auto name = [&](int i) { return i >= 0 ? _sim.node((size_t)i).name() : std::string("?"); };
  char buf[256];
  snprintf(buf, sizeof(buf), "#%llu t=%.3fs %s%s%s>%s %s%s%s %s %ums", (unsigned long long)w.seq, w.t_ms / 1000.0,
           name(w.tx_node).c_str(), w.original() ? "" : "(fwd ", w.original() ? "" : (name(w.origin) + ")").c_str(),
           name(w.dest).c_str(), kindName(w.kind), w.inner != Kind::UNKNOWN ? "/" : "",
           w.inner != Kind::UNKNOWN ? kindName(w.inner) : "", w.flood ? "flood" : "direct", (unsigned)w.airtime_ms);
  std::string s = buf;
  if (w.kind == Kind::DM || w.kind == Kind::CTRL) {
    snprintf(buf, sizeof(buf), " ts=%u attempt=%u cap=%d \"%.24s\"", (unsigned)w.ts, (unsigned)w.attempt, (int)w.cap,
             w.text.c_str());
    s += buf;
  }
  if (!w.ack.empty()) {
    s += " ack=";
    for (uint8_t b : w.ack) {
      snprintf(buf, sizeof(buf), "%02x", b);
      s += buf;
    }
  }
  return s;
}

std::string WireLog::dump(uint64_t from_ms, uint64_t to_ms) {
  std::string s;
  for (const Wire& w : all()) {
    if (w.t_ms >= from_ms && w.t_ms < to_ms) s += describe(w) + "\n";
  }
  return s;
}

// ---- hooks and helpers ----------------------------------------------------------------------------------

void onTransmit(Simulator& sim, std::function<void(const TxRecord&)> fn) {
  auto last = std::make_shared<int64_t>(-1);
  sim.add_drop_filter([last, fn](const TxRecord& tx, int) {
    if ((int64_t)tx.seq != *last) {
      *last = (int64_t)tx.seq;
      fn(tx);
    }
    return false;
  });
}

uint32_t expectAckR(uint32_t ts, uint8_t attempt, const std::string& text, const uint8_t sender_pub[32]) {
  uint8_t a[4];
  rdm::crypto::ackR(a, ts, (uint8_t)(attempt & 3), text.data(), text.size(), sender_pub);
  uint32_t v;
  memcpy(&v, a, 4);
  return v;
}

std::vector<uint8_t> expectAckS(uint32_t ts, const std::string& text, const uint8_t sender_pub[32]) {
  std::vector<uint8_t> a(6);
  rdm::crypto::ackS(a.data(), ts, 0, text.data(), text.size(), sender_pub);
  return a;
}

std::vector<uint8_t> expectKey(uint32_t ts, const std::string& text, const uint8_t sender_pub[32]) {
  std::vector<uint8_t> k(4);
  rdm::crypto::key(k.data(), ts, text.data(), text.size(), sender_pub);
  return k;
}

uint32_t ackValue(const std::vector<uint8_t>& ack) {
  uint32_t v = 0;
  if (ack.size() >= 4) memcpy(&v, ack.data(), 4);
  return v;
}

RecordDump readRecords(const SimFS& fs, const char* path) {
  RecordDump d;
  auto it = fs.files.find(path);
  if (it == fs.files.end() || it->second.size() < 16 || memcmp(it->second.data(), "RDMF", 4) != 0) return d;
  const uint8_t* h = it->second.data();
  uint8_t ver = h[4];
  bool ab = h[5] != 0;
  d.payload_size = (uint16_t)(h[6] | h[7] << 8);
  d.slots = (uint16_t)(h[8] | h[9] << 8);

  SimFS copy = fs;
  copy.write_budget = -1;
  SimFileIO io(copy);
  rdm::RecordFile f(io, path, ver, d.payload_size, d.slots, ab);
  if (f.open() != rdm::RecordFile::Open::OPENED) return d;
  std::vector<uint8_t> buf(d.payload_size);
  for (uint16_t i = 0; i < d.slots; i++) {
    if (f.read(i, buf.data())) d.records.push_back(buf);
  }
  d.ok = true;
  return d;
}

bool readContactRdm(const SimFS& fs, const uint8_t pub_prefix[6], ContactRdmView& out) {
  RecordDump d = readRecords(fs, "/rdm/contacts");
  if (!d.ok || d.payload_size != 52) return false;
  for (const auto& r : d.records) {
    if (memcmp(r.data(), pub_prefix, 6) != 0) continue;
    out.flags = r[6];
    out.watermark = le32(&r[8]);
    memcpy(out.mbx_pub, &r[12], 32);
    memcpy(out.token, &r[44], 8);
    return true;
  }
  return false;
}

bool fileContains(const SimFS& fs, const char* path, const std::string& needle) {
  auto it = fs.files.find(path);
  if (it == fs.files.end() || needle.empty()) return false;
  return std::search(it->second.begin(), it->second.end(), needle.begin(), needle.end()) != it->second.end();
}

::testing::AssertionResult airtimeWithin(uint64_t measured_ms, double expected_s, double tolerance) {
  double measured_s = measured_ms / 1000.0;
  double dev = (measured_s - expected_s) / expected_s;
  char buf[160];
  snprintf(buf, sizeof(buf), "airtime %.2f s against %.2f s: %+.0f%% (norm +-%.0f%%)", measured_s, expected_s,
           dev * 100, tolerance * 100);
  if (dev > tolerance || dev < -tolerance) return ::testing::AssertionFailure() << buf;
  return ::testing::AssertionSuccess() << buf;
}

}
