// Only in builds with reliable DM; the mailbox build (WITH_DM_MAILBOX alone) compiles helpers/rdm/*.cpp too.
#ifdef WITH_RELIABLE_DM

#include "RdmChatMesh.h"

#include "RdmCodec.h"

#include <string.h>

// Same defaults as BaseChatMesh.cpp, so fork ACKs and replies keep upstream timing.
#ifndef TXT_ACK_DELAY
  #define TXT_ACK_DELAY           200
#endif
#ifndef SERVER_RESPONSE_DELAY
  #define SERVER_RESPONSE_DELAY   300
#endif

RdmChatMesh::RdmChatMesh(mesh::Radio& radio, mesh::MillisecondClock& ms, mesh::RNG& rng, mesh::RTCClock& rtc,
                         mesh::PacketManager& mgr, mesh::MeshTables& tables, rdm::FileIO& io)
    : BaseChatMesh(radio, ms, rng, rtc, mgr, tables), _rdm(io, *this) {
  memset(_hp, 0, sizeof(_hp));
  memset(_hidden_match, 0, sizeof(_hidden_match));
}

rdm::Node& RdmChatMesh::rdm() { return _rdm; }

// ---- hidden peers (mailboxes) ----

RdmChatMesh::HiddenPeer& RdmChatMesh::hiddenPeer(const uint8_t pub[32]) {
  for (auto& h : _hp)
    if (h.used && memcmp(h.pub, pub, 32) == 0) return h;
  HiddenPeer& h = _hp[_hp_next];
  _hp_next = (uint8_t)((_hp_next + 1) % RDM_HIDDEN_PEERS);
  h.used = true;
  memcpy(h.pub, pub, 32);
  self_id.calcSharedSecret(h.secret, pub);
  h.out_path_len = OUT_PATH_UNKNOWN;
  return h;
}

const uint8_t* RdmChatMesh::matchedHiddenPub(int peer_idx) const {
  int k = peer_idx - _n_base_match;
  if (k < 0 || k >= RDM_HIDDEN_PEERS) return NULL;
  return _rdm.hiddenPeerPub(_hidden_match[k]);
}

int RdmChatMesh::searchPeersByHash(const uint8_t* hash) {
  int n = BaseChatMesh::searchPeersByHash(hash);
  _n_base_match = n;
  uint8_t k = 0;
  for (uint8_t i = 0; i < _rdm.hiddenPeerCount() && k < RDM_HIDDEN_PEERS; i++) {
    const uint8_t* pub = _rdm.hiddenPeerPub(i);
    if (pub && memcmp(pub, hash, PATH_HASH_SIZE) == 0) _hidden_match[k++] = i;
  }
  return n + k;
}

void RdmChatMesh::getPeerSharedSecret(uint8_t* dest_secret, int peer_idx) {
  const uint8_t* pub = peer_idx >= _n_base_match ? matchedHiddenPub(peer_idx) : NULL;
  if (pub) {
    memcpy(dest_secret, hiddenPeer(pub).secret, PUB_KEY_SIZE);
  } else {
    BaseChatMesh::getPeerSharedSecret(dest_secret, peer_idx);
  }
}

// ---- receiving ----

void RdmChatMesh::onPeerDataRecv(mesh::Packet* pkt, uint8_t type, int sender_idx, const uint8_t* secret, uint8_t* data,
                                 size_t len) {
  if (sender_idx >= _n_base_match) {
    const uint8_t* pub = matchedHiddenPub(sender_idx);
    if (pub && type == PAYLOAD_TYPE_RESPONSE) _rdm.onResponse(pub, data, len);
    return;
  }
  ContactInfo* from = rdmMatchedPeer(sender_idx);
  if (from && _rdm.enabled()) {
    if (type == PAYLOAD_TYPE_TXT_MSG && len > 5) {
      uint8_t txt_type = data[4] >> 2;
      if (txt_type == TXT_TYPE_PLAIN) {
        handleTxtPlain(pkt, *from, secret, data, len);
        return;
      }
      if (txt_type == rdm::TXT_TYPE_RDM_CTRL) {
        from->lastmod = getRTCClock()->getCurrentTime();
        _rdm.onCtrlTxt(from->id.pub_key, data, len);
        return;
      }
    } else if (type == PAYLOAD_TYPE_REQ && len > 4 && data[4] == rdm::REQ_RECEIPT_QUERY) {
      uint8_t reply[4 + 1 + rdm::MAX_BATCH * 7];
      memcpy(reply, data, 4);   // tag = sender timestamp
      uint8_t n = _rdm.onReceiptQuery(from->id.pub_key, &data[4], len - 4, &reply[4]);
      if (n) sendReply(pkt, *from, secret, reply, (uint8_t)(4 + n));
      return;
    } else if (type == PAYLOAD_TYPE_RESPONSE && _rdm.onResponse(from->id.pub_key, data, len)) {
      if (pkt->isRouteFlood() && from->out_path_len != OUT_PATH_UNKNOWN) {
        handleReturnPathRetry(*from, pkt->path, pkt->path_len);
      }
      return;
    }
  }
  BaseChatMesh::onPeerDataRecv(pkt, type, sender_idx, secret, data, len);
}

// Upstream flow of BaseChatMesh::onPeerDataRecv for TXT_TYPE_PLAIN, with the inbox deciding whether to ACK:
// no ACK before the message is stored (#3518), ACK_R with cap byte, ACK_S for a synced duplicate.
void RdmChatMesh::handleTxtPlain(mesh::Packet* pkt, ContactInfo& from, const uint8_t* secret, uint8_t* data,
                                 size_t len) {
  uint8_t path_len = pkt->isRouteFlood() ? pkt->path_len : 0xFF;
  rdm::RecvDecision d = _rdm.onPlainTxt(from.id.pub_key, data, len, path_len, pkt->_snr);
  if (d.result == rdm::RecvResult::NEW || d.result == rdm::RecvResult::DUPLICATE) {
    from.lastmod = getRTCClock()->getCurrentTime();
  }
  if (d.send_ack_r) {
    rdm::codec::TxtParsed p;
    uint8_t ext = rdm::codec::parseTxtPlain(data, len, p) ? p.ext_attempt : 0;
    uint8_t rnd;
    getRNG()->random(&rnd, 1);
    uint8_t ack[7];
    size_t n = rdm::codec::buildAckR(ack, d.ack_r, ext, rnd, d.cap_byte);
    if (pkt->isRouteFlood()) {
      // the path TO here lets the sender go direct, and carries the ACK
      mesh::Packet* path = createPathReturn(from.id, secret, pkt->path, pkt->path_len, PAYLOAD_TYPE_ACK, ack, n);
      if (path) sendFloodScoped(from, path, TXT_ACK_DELAY);
    } else {
      rdmSendAckTo(from, ack, (uint8_t)n);
    }
  }
  if (d.send_ack_s) rdmSendAckTo(from, d.ack_s, 6);
}

void RdmChatMesh::sendReply(mesh::Packet* pkt, ContactInfo& from, const uint8_t* secret, const uint8_t* reply,
                            uint8_t len) {
  if (pkt->isRouteFlood()) {
    mesh::Packet* path = createPathReturn(from.id, secret, pkt->path, pkt->path_len, PAYLOAD_TYPE_RESPONSE, reply, len);
    if (path) sendFloodScoped(from, path, SERVER_RESPONSE_DELAY);
    return;
  }
  mesh::Packet* r = createDatagram(PAYLOAD_TYPE_RESPONSE, from.id, secret, reply, len);
  if (!r) return;
  if (from.out_path_len != OUT_PATH_UNKNOWN) {
    sendDirect(r, from.out_path, from.out_path_len, SERVER_RESPONSE_DELAY);
  } else {
    sendFloodScoped(from, r, SERVER_RESPONSE_DELAY);
  }
}

bool RdmChatMesh::onPeerPathRecv(mesh::Packet* pkt, int sender_idx, const uint8_t* secret, uint8_t* path,
                                 uint8_t path_len, uint8_t extra_type, uint8_t* extra, uint8_t extra_len) {
  if (sender_idx >= _n_base_match) {
    const uint8_t* pub = matchedHiddenPub(sender_idx);
    if (!pub) return false;
    HiddenPeer& h = hiddenPeer(pub);
    h.out_path_len = mesh::Packet::copyPath(h.out_path, path, path_len);
    if (extra_type == PAYLOAD_TYPE_RESPONSE && extra_len > 0) _rdm.onResponse(pub, extra, extra_len);
    return true;   // reciprocal path to the mailbox
  }
  return BaseChatMesh::onPeerPathRecv(pkt, sender_idx, secret, path, path_len, extra_type, extra, extra_len);
}

// PATH extra is padded with zeroes to the cipher block: an ACK is 7 bytes with the cap byte, otherwise 6.
static uint8_t ackExtraLen(const uint8_t* extra, uint8_t len) {
  if (len >= 7 && extra[6] == rdm::CAP_BYTE) return 7;
  return len < 6 ? len : 6;
}

bool RdmChatMesh::onContactPathRecv(ContactInfo& from, uint8_t* in_path, uint8_t in_path_len, uint8_t* out_path,
                                    uint8_t out_path_len, uint8_t extra_type, uint8_t* extra, uint8_t extra_len) {
  if (_rdm.enabled()) {
    bool mine = false;
    if (extra_type == PAYLOAD_TYPE_ACK && extra_len >= 4) {
      mine = _rdm.onAck(extra, ackExtraLen(extra, extra_len));
    } else if (extra_type == PAYLOAD_TYPE_RESPONSE && extra_len > 0) {
      mine = _rdm.onResponse(from.id.pub_key, extra, extra_len);
    }
    if (mine) return BaseChatMesh::onContactPathRecv(from, in_path, in_path_len, out_path, out_path_len, 0, NULL, 0);
  }
  return BaseChatMesh::onContactPathRecv(from, in_path, in_path_len, out_path, out_path_len, extra_type, extra, extra_len);
}

void RdmChatMesh::onAckRecv(mesh::Packet* pkt, uint32_t ack_crc) {
  if (_rdm.enabled()) {
    uint8_t len = pkt->payload_len > 7 ? 7 : (uint8_t)pkt->payload_len;
    if (len >= 4 && _rdm.onAck(pkt->payload, len)) {
      pkt->markDoNotRetransmit();   // ACK was for this node
      return;
    }
  }
  BaseChatMesh::onAckRecv(pkt, ack_crc);
}

// K3: a mailbox is infrastructure, not a contact. Its advert only feeds the RDM triggers; auto-add would
// otherwise put it in contacts[] (every type is auto-added by default).
void RdmChatMesh::onAdvertRecv(mesh::Packet* pkt, const mesh::Identity& id, uint32_t ts, const uint8_t* app_data,
                               size_t len) {
  for (uint8_t i = 0; i < _rdm.hiddenPeerCount(); i++) {
    const uint8_t* pub = _rdm.hiddenPeerPub(i);
    if (pub && memcmp(pub, id.pub_key, PUB_KEY_SIZE) == 0) {
      _rdm.onAdvert(id.pub_key);
      return;
    }
  }
  BaseChatMesh::onAdvertRecv(pkt, id, ts, app_data, len);
  _rdm.onAdvert(id.pub_key);
}

// A contact that became a hidden peer leaves contacts[], favourite or not; its path is not carried over, the
// route to the mailbox comes from its PATH returns. Idempotent: nothing left to remove on a repeat.
void RdmChatMesh::onHiddenPeersChanged() {
  for (uint8_t i = 0; i < _rdm.hiddenPeerCount(); i++) {
    const uint8_t* pub = _rdm.hiddenPeerPub(i);
    ContactInfo* c = pub ? lookupContactByPubKey(pub, PUB_KEY_SIZE) : NULL;
    if (!c) continue;
    ContactInfo removed = *c;
    if (removeContact(*c)) rdmOnContactRemoved(removed);
  }
}

// ---- NodeHost, mesh side ----

uint32_t RdmChatMesh::millis() { return _ms->getMillis(); }

uint32_t RdmChatMesh::random32() {
  uint32_t v;
  getRNG()->random((uint8_t*)&v, sizeof(v));
  return v;
}

bool RdmChatMesh::txIdle() { return _mgr->getOutboundTotal() == 0; }

void RdmChatMesh::selfPub(uint8_t pub_out[32]) { memcpy(pub_out, self_id.pub_key, PUB_KEY_SIZE); }

bool RdmChatMesh::lookupContact(const uint8_t pub_prefix[6], uint8_t pub_out[32], bool& favourite, bool& has_path) {
  ContactInfo* c = lookupContactByPubKey(pub_prefix, 6);
  if (!c) return false;
  memcpy(pub_out, c->id.pub_key, PUB_KEY_SIZE);
  favourite = c->isFav();
  has_path = c->out_path_len != OUT_PATH_UNKNOWN;
  return true;
}

// flood = true forces flood; otherwise direct whenever a path is known (contact or mailbox).
void RdmChatMesh::sendToPeer(mesh::Packet* pkt, const uint8_t peer_pub[32], bool flood, uint32_t& est_timeout_ms) {
  uint32_t t = _radio->getEstAirtimeFor(pkt->getRawLength());
  ContactInfo* c = lookupContactByPubKey(peer_pub, PUB_KEY_SIZE);
  const uint8_t* path = NULL;
  uint8_t path_len = OUT_PATH_UNKNOWN;
  if (c) {
    path = c->out_path;
    path_len = c->out_path_len;
  } else {
    HiddenPeer& h = hiddenPeer(peer_pub);
    path = h.out_path;
    path_len = h.out_path_len;
  }
  if (flood || path_len == OUT_PATH_UNKNOWN) {
    if (c) {
      sendFloodScoped(*c, pkt);
    } else {
      ContactInfo tmp;
      memset(&tmp, 0, sizeof(tmp));
      tmp.id = mesh::Identity(peer_pub);
      tmp.out_path_len = OUT_PATH_UNKNOWN;
      sendFloodScoped(tmp, pkt);
    }
    est_timeout_ms = calcFloodTimeoutMillisFor(t);
  } else {
    sendDirect(pkt, path, path_len);
    est_timeout_ms = calcDirectTimeoutMillisFor(t, path_len);
  }
}

bool RdmChatMesh::sendTxtPlain(const uint8_t pub_prefix[6], const uint8_t* plain, size_t len, bool flood,
                               uint32_t& est_timeout_ms) {
  ContactInfo* c = lookupContactByPubKey(pub_prefix, 6);
  if (!c) return false;
  mesh::Packet* pkt = createDatagram(PAYLOAD_TYPE_TXT_MSG, c->id, c->getSharedSecret(self_id), plain, len);
  if (!pkt) return false;
  sendToPeer(pkt, c->id.pub_key, flood, est_timeout_ms);
  return true;
}

bool RdmChatMesh::sendAck(const uint8_t pub_prefix[6], const uint8_t* ack, uint8_t len) {
  ContactInfo* c = lookupContactByPubKey(pub_prefix, 6);
  if (!c) return false;
  rdmSendAckTo(*c, ack, len);
  return true;
}

bool RdmChatMesh::sendReq(const uint8_t peer_pub[32], uint32_t req_ts, const uint8_t* body, size_t len, bool flood,
                          uint32_t& est_timeout_ms) {
  uint8_t data[MAX_PACKET_PAYLOAD];
  if (len + 4 > sizeof(data)) return false;
  memcpy(data, &req_ts, 4);
  memcpy(&data[4], body, len);
  ContactInfo* c = lookupContactByPubKey(peer_pub, PUB_KEY_SIZE);
  const uint8_t* secret = c ? c->getSharedSecret(self_id) : hiddenPeer(peer_pub).secret;
  mesh::Packet* pkt = createDatagram(PAYLOAD_TYPE_REQ, mesh::Identity(peer_pub), secret, data, len + 4);
  if (!pkt) return false;
  sendToPeer(pkt, peer_pub, flood, est_timeout_ms);
  return true;
}

bool RdmChatMesh::sendAnonReq(const uint8_t peer_pub[32], const uint8_t* plain, size_t len, uint32_t& est_timeout_ms) {
  mesh::Packet* pkt = createAnonDatagram(PAYLOAD_TYPE_ANON_REQ, self_id, mesh::Identity(peer_pub),
                                         hiddenPeer(peer_pub).secret, plain, len);
  if (!pkt) return false;
  sendToPeer(pkt, peer_pub, false, est_timeout_ms);
  return true;
}

bool RdmChatMesh::encryptTxtPayload(const uint8_t pub_prefix[6], const uint8_t* plain, size_t len, uint8_t* payload_out,
                                    size_t& payload_len) {
  ContactInfo* c = lookupContactByPubKey(pub_prefix, 6);
  if (!c) return false;
  mesh::Packet* pkt = createDatagram(PAYLOAD_TYPE_TXT_MSG, c->id, c->getSharedSecret(self_id), plain, len);
  if (!pkt) return false;
  bool ok = pkt->payload_len <= payload_len;
  if (ok) {
    memcpy(payload_out, pkt->payload, pkt->payload_len);
    payload_len = pkt->payload_len;
  }
  releasePacket(pkt);
  return ok;
}

// Same matching as Mesh::onRecvPacket: 1-byte hashes, then the MAC decides which contact sent it.
bool RdmChatMesh::decryptTxtPayload(const uint8_t* payload, size_t len, uint8_t sender_pub_out[32], uint8_t* plain_out,
                                    size_t& plain_len) {
  if (len < 2 + CIPHER_MAC_SIZE + CIPHER_BLOCK_SIZE || !self_id.isHashMatch(payload) || plain_len < len) return false;
  ContactInfo c;
  for (int i = 0; i < getTotalContactSlots(); i++) {
    if (!getContactByIdx(i, c) || !c.id.isHashMatch(&payload[1])) continue;
    int n = mesh::Utils::MACThenDecrypt(c.getSharedSecret(self_id), plain_out, &payload[2], (int)len - 2);
    if (n > 0) {
      memcpy(sender_pub_out, c.id.pub_key, PUB_KEY_SIZE);
      plain_len = (size_t)n;
      return true;
    }
  }
  return false;
}

#endif
