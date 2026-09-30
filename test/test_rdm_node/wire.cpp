#include "wire.h"

namespace wire {

Peer::Peer(Net& n, int i, uint32_t capacity) : net(n), id(i), io(capacity) {
  for (int k = 0; k < 32; k++) pub[k] = (uint8_t)(0x11 * (i + 1) + k * 7);
}

Peer::Contact* Peer::contactOf(const uint8_t* prefix) {
  for (auto& c : contacts)
    if (memcmp(net.peers[c.peer]->pub, prefix, 6) == 0) return &c;
  return nullptr;
}

uint32_t Peer::millis() { return net.ms; }

uint32_t Peer::rtcNow(bool& trusted) {
  trusted = rtc_trusted;
  return net.epoch + net.ms / 1000;
}

bool Peer::lookupContact(const uint8_t prefix[6], uint8_t pub_out[32], bool& fav, bool& has_path) {
  Contact* c = contactOf(prefix);
  if (!c) return false;
  memcpy(pub_out, net.peers[c->peer]->pub, 32);
  fav = c->fav;
  has_path = c->path;
  return true;
}

bool Peer::sendTxtPlain(const uint8_t prefix[6], const uint8_t* plain, size_t len, bool flood, uint32_t& est) {
  (void)flood;
  Peer* to = net.byPrefix(prefix);
  if (!to) return false;
  est = 2000;
  net.send(Kind::TXT, id, to->id, plain, len);
  return true;
}

bool Peer::sendAck(const uint8_t prefix[6], const uint8_t* ack, uint8_t len) {
  Peer* to = net.byPrefix(prefix);
  if (!to) return false;
  net.send(Kind::ACK, id, to->id, ack, len);
  return true;
}

bool Peer::sendReq(const uint8_t peer_pub[32], uint32_t ts, const uint8_t* body, size_t len, bool flood, uint32_t& est) {
  (void)flood;
  Peer* to = net.byPrefix(peer_pub, 32);
  if (!to) return false;
  std::vector<uint8_t> d(4 + len);
  memcpy(d.data(), &ts, 4);
  memcpy(d.data() + 4, body, len);
  est = 2000;
  net.send(Kind::REQ, id, to->id, d.data(), d.size());
  return true;
}

bool Peer::sendAnonReq(const uint8_t peer_pub[32], const uint8_t* plain, size_t len, uint32_t& est) {
  Peer* to = net.byPrefix(peer_pub, 32);
  if (!to) return false;
  est = 2000;
  net.send(Kind::ANON, id, to->id, plain, len);
  return true;
}

// Fake cipher: dest(1) | src(1) | MAC(2) = 0 | plaintext padded to 16. Enough to route and to bind the sender.
bool Peer::encryptTxtPayload(const uint8_t prefix[6], const uint8_t* plain, size_t len, uint8_t* out, size_t& out_len) {
  Peer* to = net.byPrefix(prefix);
  size_t padded = (len + 15) / 16 * 16;
  if (!to || 4 + padded > out_len) return false;
  out[0] = to->pub[0];
  out[1] = pub[0];
  out[2] = out[3] = 0;
  memset(out + 4, 0, padded);
  memcpy(out + 4, plain, len);
  out_len = 4 + padded;
  return true;
}

bool Peer::decryptTxtPayload(const uint8_t* payload, size_t len, uint8_t sender_out[32], uint8_t* plain, size_t& plain_len) {
  if (len < 4 || payload[0] != pub[0] || payload[2] || payload[3]) return false;
  for (auto& c : contacts) {
    Peer& p = *net.peers[c.peer];
    if (p.pub[0] != payload[1]) continue;
    memcpy(sender_out, p.pub, 32);
    plain_len = len - 4;
    memcpy(plain, payload + 4, plain_len);
    return true;
  }
  return false;
}

bool Peer::pushUserStatus(const uint8_t prefix[6], uint32_t app_ack, UserStatus s, const uint8_t key[4], uint32_t ts) {
  (void)prefix;
  (void)key;
  if (!rdm_client) return false;
  statuses.push_back({app_ack, s, ts});
  return true;
}

Node::AppSend Peer::appSend(Peer& to, const std::string& text, uint8_t attempt, uint32_t ts) {
  if (!ts) ts = net.epoch + net.ms / 1000;
  Node::AppSend a = node->appSend(to.pub, ts, attempt, text.data(), text.size());
  if (a.handled) return a;
  uint8_t plain[200];
  size_t n = codec::buildTxtPlain(plain, ts, 0, attempt, text.data(), text.size(), false);
  uint32_t est = 0;
  if (n) sendTxtPlain(to.pub, plain, n, false, est);
  return a;
}

std::vector<std::string> Peer::appSync() {
  std::vector<std::string> out;
  for (;;) {
    node->onSyncRequest();
    InRecord r;
    uint16_t slot;
    if (!node->nextInboxFrame(r, slot)) break;
    out.emplace_back(r.text, r.text_len);
    node->onInboxFrameHanded(slot);
  }
  return out;
}

bool Peer::hasStatus(UserStatus s) const {
  for (const auto& p : statuses)
    if (p.s == s) return true;
  return false;
}

Peer* Net::byPrefix(const uint8_t* prefix, size_t n) {
  for (auto& p : peers)
    if (memcmp(p->pub, prefix, n) == 0) return p.get();
  return nullptr;
}

void Net::send(Kind k, int from, int to, const uint8_t* data, size_t len) {
  Msg m{k, from, to, std::vector<uint8_t>(data, data + len), ms};
  log.push_back(m);
  queue.push_back(m);
}

void Net::pump() {
  while (!queue.empty()) {
    Msg m = queue.front();
    queue.pop_front();
    Peer& to = *peers[m.to];
    Peer& from = *peers[m.from];
    if (!to.online || (drop && drop(m))) continue;
    const uint8_t* d = m.bytes.data();
    size_t len = m.bytes.size();
    if (to.on_req && (m.kind == Kind::REQ || m.kind == Kind::ANON)) {
      to.on_req(to, m);
      continue;
    }
    switch (m.kind) {
      case Kind::TXT: {
        uint8_t txt_type = d[4] >> 2;
        if (txt_type == TXT_TYPE_RDM_CTRL) {
          to.node->onCtrlTxt(from.pub, d, len);
          break;
        }
        RecvDecision dec = to.node->onPlainTxt(from.pub, d, len, 0xFF, 0);
        if (dec.send_ack_r) {
          codec::TxtParsed p;
          codec::parseTxtPlain(d, len, p);
          uint8_t ack[7];
          size_t n = codec::buildAckR(ack, dec.ack_r, p.ext_attempt, 0x5A, dec.cap_byte);
          send(Kind::ACK, to.id, from.id, ack, n);
        }
        if (dec.send_ack_s) send(Kind::ACK, to.id, from.id, dec.ack_s, 6);
        break;
      }
      case Kind::ACK:
        to.node->onAck(d, (uint8_t)len);
        break;
      case Kind::REQ:
        if (len > 4 && d[4] == REQ_RECEIPT_QUERY) {
          uint8_t reply[64];
          memcpy(reply, d, 4);
          uint8_t n = to.node->onReceiptQuery(from.pub, d + 4, len - 4, reply + 4);
          if (n) send(Kind::RESP, to.id, from.id, reply, 4 + n);
        }
        break;
      case Kind::RESP:
        to.node->onResponse(from.pub, d, len);
        break;
      case Kind::ANON:
        break;
    }
  }
}

void Net::run(uint32_t total_ms, uint32_t step_ms) {
  uint32_t end = ms + total_ms;
  while (ms < end) {
    ms += step_ms;
    for (auto& p : peers)
      if (p->online && p->node) p->node->loop();
    pump();
  }
}

size_t Net::count(Kind k, int from, int to, int first_byte) const {
  size_t n = 0;
  for (const auto& m : log) {
    if (m.kind != k || (from >= 0 && m.from != from) || (to >= 0 && m.to != to)) continue;
    if (first_byte >= 0 && (m.bytes.size() < 5 || m.bytes[4] != first_byte)) continue;
    n++;
  }
  return n;
}

}
