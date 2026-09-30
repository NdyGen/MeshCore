#pragma once

#include <Mesh.h>
#include <helpers/TransportKeyStore.h>
#include <helpers/rdm/RdmConfig.h>
#include <helpers/rdm/mailbox/MailboxCore.h>

#ifndef MBX_PEER_CACHE_SIZE
  #define MBX_PEER_CACHE_SIZE  RDM_MBX_CLIENTS
#endif
static_assert(MBX_PEER_CACHE_SIZE == RDM_MBX_CLIENTS, "MailboxCore rate-limits exactly the peers the cache can decrypt");
#ifndef MBX_FLOOD_ADVERT_INTERVAL_S
  #define MBX_FLOOD_ADVERT_INTERVAL_S  (12UL * 3600)
#endif
#ifndef MBX_BOOT_ADVERT_DELAY_MS
  #define MBX_BOOT_ADVERT_DELAY_MS  30000
#endif
#ifndef MBX_RESPONSE_DELAY_MS
  #define MBX_RESPONSE_DELAY_MS  300     // as SERVER_RESPONSE_DELAY in simple_room_server
#endif
// MBX_FLOOD_SCOPE: region name. Floods (adverts, replies without a known path) then carry that region's transport
// code, like the companion's default scope, for repeaters that refuse unscoped floods. Unset: unscoped.

// Mailbox role (03 par. 7): answers registrations (ANON_REQ) and DEPOSIT/FETCH/STATUS requests through
// rdm::MailboxCore, which talks to mbxd via a MailboxBackend. No chat, no forwarding, no contact list: peers live
// in an LRU cache that the backend's ACL fills after boot and registrations extend.
class MailboxMesh : public mesh::Mesh, private rdm::MailboxCoreHost {
public:
  MailboxMesh(mesh::Radio& radio, mesh::MillisecondClock& ms, mesh::RNG& rng, mesh::RTCClock& rtc,
              mesh::PacketManager& mgr, mesh::MeshTables& tables, rdm::MailboxBackend& backend, const char* name);

  void begin();
  void loop();
  // Milliseconds until loop() has timed work (core timeouts and paced responses, next advert); UINT32_MAX: none.
  uint32_t nextWakeupMillis();
  void sendSelfAdvert(bool flood);
  void setFloodScope(const char* region);   // nullptr: unscoped

  uint8_t peerCount() const;
  bool    knowsPeer(const uint8_t pub[32]) const;

protected:
  bool allowPacketForward(const mesh::Packet* packet) override { return false; }
  void onAnonDataRecv(mesh::Packet* packet, const uint8_t* secret, const mesh::Identity& sender, uint8_t* data,
                      size_t len) override;
  int  searchPeersByHash(const uint8_t* hash) override;
  void getPeerSharedSecret(uint8_t* dest_secret, int peer_idx) override;
  void onPeerDataRecv(mesh::Packet* packet, uint8_t type, int sender_idx, const uint8_t* secret, uint8_t* data,
                      size_t len) override;
  bool onPeerPathRecv(mesh::Packet* packet, int sender_idx, const uint8_t* secret, uint8_t* path, uint8_t path_len,
                      uint8_t extra_type, uint8_t* extra, uint8_t extra_len) override;

private:
  static const uint8_t PATH_UNKNOWN = 0xFF;

  struct Peer {
    bool          used;
    bool          confirmed;             // from the ACL or a registration OK; unconfirmed ones are evicted first
    mesh::Identity id;
    uint8_t       secret[PUB_KEY_SIZE];
    uint8_t       out_path[MAX_PATH_SIZE];
    uint8_t       out_path_len;          // PATH_UNKNOWN until the client sends its reciprocal PATH
    // Path of the latest flood request: a PATH return has to carry it back.
    bool          in_path_valid;
    uint8_t       in_path[MAX_PATH_SIZE];
    uint8_t       in_path_len;
    uint8_t       in_hash_size;
    uint32_t      lru;
  };

  rdm::MailboxCore _core;
  const char*   _name;
  Peer          _peers[MBX_PEER_CACHE_SIZE];
  int           _matching[MBX_PEER_CACHE_SIZE];
  uint32_t      _lru_seq = 0;
  unsigned long _next_advert = 0;
  TransportKey  _scope;                  // all zero: unscoped

  Peer* findPeer(const uint8_t pub[32]);
  Peer* insertPeer(const uint8_t pub[32], bool confirmed);
  void  notePeerRoute(Peer& p, const mesh::Packet* packet);
  void  floodOut(mesh::Packet* pkt, uint32_t delay_millis, uint8_t path_hash_size);

  // MailboxCoreHost
  uint32_t millis() override;
  bool txIdle() override;
  bool addPeer(const uint8_t pub[32]) override;
  bool sendResponse(const uint8_t client_pub[32], uint32_t tag, const uint8_t* body, size_t len,
                    bool path_return) override;
};
