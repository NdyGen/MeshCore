#include "MailboxMesh.h"

#include <helpers/AdvertDataHelpers.h>
#include <helpers/rdm/RdmPolicy.h>

#include <stdio.h>
#include <string.h>

MailboxMesh::MailboxMesh(mesh::Radio& radio, mesh::MillisecondClock& ms, mesh::RNG& rng, mesh::RTCClock& rtc,
                         mesh::PacketManager& mgr, mesh::MeshTables& tables, rdm::MailboxBackend& backend,
                         const char* name)
    : mesh::Mesh(radio, ms, rng, rtc, mgr, tables), _core(backend, *this), _name(name) {
  memset(_peers, 0, sizeof(_peers));
  memset(&_scope, 0, sizeof(_scope));
#ifdef MBX_FLOOD_SCOPE
  setFloodScope(MBX_FLOOD_SCOPE);
#endif
}

void MailboxMesh::setFloodScope(const char* region) {
  memset(&_scope, 0, sizeof(_scope));
  if (!region || !*region) return;
  char name[40];
  snprintf(name, sizeof(name), "#%s", region);   // same key derivation as the companion's DEFAULT_FLOOD_SCOPE_NAME
  TransportKeyStore keys;
  keys.getAutoKeyFor(0, name, _scope);
}

void MailboxMesh::floodOut(mesh::Packet* pkt, uint32_t delay_millis, uint8_t path_hash_size) {
  if (_scope.isNull()) {
    sendFlood(pkt, delay_millis, path_hash_size);
  } else {
    uint16_t codes[2] = {_scope.calcTransportCode(pkt), 0};
    sendFlood(pkt, codes, delay_millis, path_hash_size);
  }
}

void MailboxMesh::begin() {
  mesh::Mesh::begin();
  _core.begin();
  _next_advert = futureMillis(MBX_BOOT_ADVERT_DELAY_MS);
}

void MailboxMesh::loop() {
  mesh::Mesh::loop();
  _core.loop();
  if (millisHasNowPassed(_next_advert)) {
    // Clients use M's advert as a hint to FETCH or ask STATUS, and to learn a path.
    sendSelfAdvert(true);
    // G17: I + U[0, jitterMax(I)], drawn per interval, so M's adverts do not fall in step with other timers.
    uint32_t interval_s = MBX_FLOOD_ADVERT_INTERVAL_S;
    interval_s += getRNG()->nextInt(0, rdm::jitterMax(interval_s) + 1);
    _next_advert = futureMillis((int)(interval_s * 1000UL));
  }
}

uint32_t MailboxMesh::nextWakeupMillis() {
  uint32_t now = millis();
  uint32_t w = _core.nextWakeupMillis(now);
  uint32_t advert = millisHasNowPassed(_next_advert) ? 0 : (uint32_t)(_next_advert - now);
  return advert < w ? advert : w;
}

void MailboxMesh::sendSelfAdvert(bool flood) {
  uint8_t app_data[MAX_ADVERT_DATA_SIZE];
  AdvertDataBuilder builder(ADV_TYPE_ROOM, _name);
  uint8_t len = builder.encodeTo(app_data);
  mesh::Packet* pkt = createAdvert(self_id, app_data, len);
  if (!pkt) return;
  if (flood) {
    floodOut(pkt, 0, 1);
  } else {
    sendZeroHop(pkt);
  }
}

uint8_t MailboxMesh::peerCount() const {
  uint8_t n = 0;
  for (auto& p : _peers) if (p.used) n++;
  return n;
}

bool MailboxMesh::knowsPeer(const uint8_t pub[32]) const {
  for (auto& p : _peers)
    if (p.used && memcmp(p.id.pub_key, pub, PUB_KEY_SIZE) == 0) return true;
  return false;
}

MailboxMesh::Peer* MailboxMesh::findPeer(const uint8_t pub[32]) {
  for (auto& p : _peers)
    if (p.used && memcmp(p.id.pub_key, pub, PUB_KEY_SIZE) == 0) return &p;
  return nullptr;
}

MailboxMesh::Peer* MailboxMesh::insertPeer(const uint8_t pub[32], bool confirmed, const uint8_t* secret) {
  Peer* victim = nullptr;
  for (auto& p : _peers) {
    if (!p.used) { victim = &p; break; }
    // A burst of unknown senders must not push out registered depositors and owners.
    if (!victim || (victim->confirmed && !p.confirmed) || (victim->confirmed == p.confirmed && p.lru < victim->lru))
      victim = &p;
  }
  memset(victim, 0, sizeof(*victim));
  victim->used = true;
  victim->confirmed = confirmed;
  victim->id = mesh::Identity(pub);
  if (secret) {
    memcpy(victim->secret, secret, PUB_KEY_SIZE);
    victim->has_secret = true;
  }
  victim->out_path_len = PATH_UNKNOWN;
  victim->in_hash_size = 1;
  victim->lru = ++_lru_seq;
  return victim;
}

const uint8_t* MailboxMesh::peerSecret(Peer& p) {
  if (!p.has_secret) {
    self_id.calcSharedSecret(p.secret, p.id);
    p.has_secret = true;
  }
  return p.secret;
}

void MailboxMesh::notePeerRoute(Peer& p, const mesh::Packet* packet) {
  p.lru = ++_lru_seq;
  if (packet->isRouteFlood()) {
    p.in_path_valid = true;
    p.in_path_len = mesh::Packet::copyPath(p.in_path, packet->path, packet->path_len);
    p.in_hash_size = packet->getPathHashSize();
  }
}

void MailboxMesh::onAnonDataRecv(mesh::Packet* packet, const uint8_t* secret, const mesh::Identity& sender,
                                 uint8_t* data, size_t len) {
  if (packet->getPayloadType() != PAYLOAD_TYPE_ANON_REQ) return;
  // Kept unconfirmed so the registration reply can be encrypted and routed; confirmed once the Pi says OK.
  Peer* p = findPeer(sender.pub_key);
  if (!p) {
    p = insertPeer(sender.pub_key, false, secret);
  } else if (!p->has_secret) {
    memcpy(p->secret, secret, PUB_KEY_SIZE);
    p->has_secret = true;
  }
  notePeerRoute(*p, packet);
  _core.onAnonRequest(sender.pub_key, data, len, packet->isRouteFlood());
}

int MailboxMesh::searchPeersByHash(const uint8_t* hash) {
  int n = 0;
  for (int i = 0; i < MBX_PEER_CACHE_SIZE; i++)
    if (_peers[i].used && _peers[i].id.isHashMatch(hash)) _matching[n++] = (uint8_t)i;
  return n;
}

void MailboxMesh::getPeerSharedSecret(uint8_t* dest_secret, int peer_idx) {
  memcpy(dest_secret, peerSecret(_peers[_matching[peer_idx]]), PUB_KEY_SIZE);
}

void MailboxMesh::onPeerDataRecv(mesh::Packet* packet, uint8_t type, int sender_idx, const uint8_t* secret,
                                 uint8_t* data, size_t len) {
  if (type != PAYLOAD_TYPE_REQ) return;
  Peer& p = _peers[_matching[sender_idx]];
  notePeerRoute(p, packet);
  _core.onRequest(p.id.pub_key, data, len, packet->isRouteFlood());
}

bool MailboxMesh::onPeerPathRecv(mesh::Packet* packet, int sender_idx, const uint8_t* secret, uint8_t* path,
                                 uint8_t path_len, uint8_t extra_type, uint8_t* extra, uint8_t extra_len) {
  Peer& p = _peers[_matching[sender_idx]];
  p.out_path_len = mesh::Packet::copyPath(p.out_path, path, path_len);
  p.lru = ++_lru_seq;
  return false;   // clients only send us their reciprocal path; nothing to return
}

uint32_t MailboxMesh::millis() { return (uint32_t)_ms->getMillis(); }

bool MailboxMesh::txIdle() { return _mgr->getOutboundTotal() == 0; }

bool MailboxMesh::addPeer(const uint8_t pub[32]) {
  Peer* p = findPeer(pub);
  if (p) {
    p->confirmed = true;
    return true;
  }
  return insertPeer(pub, true) != nullptr;
}

bool MailboxMesh::sendResponse(const uint8_t client_pub[32], uint32_t tag, const uint8_t* body, size_t len,
                               bool path_return) {
  Peer* p = findPeer(client_pub);
  if (!p) p = insertPeer(client_pub, false);   // evicted between request and reply: no route left, so flood
  const uint8_t* secret = peerSecret(*p);

  uint8_t data[4 + MAX_PACKET_PAYLOAD];
  if (len > MAX_PACKET_PAYLOAD) return false;
  memcpy(data, &tag, 4);
  memcpy(&data[4], body, len);

  // The core asks for a PATH return only for a request that came by flood. Without that request's path (peer
  // evicted meanwhile) an empty path would tell the client M is its neighbour, so fall back to a datagram.
  if (path_return && p->in_path_valid) {
    mesh::Packet* path = createPathReturn(p->id, secret, p->in_path, p->in_path_len, PAYLOAD_TYPE_RESPONSE,
                                          data, 4 + len);
    if (path) {
      floodOut(path, MBX_RESPONSE_DELAY_MS, p->in_hash_size);
      return true;
    }
  }
  mesh::Packet* reply = createDatagram(PAYLOAD_TYPE_RESPONSE, p->id, secret, data, 4 + len);
  if (!reply) return false;
  if (p->out_path_len != PATH_UNKNOWN) {
    sendDirect(reply, p->out_path, p->out_path_len, MBX_RESPONSE_DELAY_MS);
  } else {
    floodOut(reply, MBX_RESPONSE_DELAY_MS, p->in_hash_size);
  }
  return true;
}
