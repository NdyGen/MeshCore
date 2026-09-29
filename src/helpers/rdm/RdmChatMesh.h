#pragma once

#include <helpers/BaseChatMesh.h>
#include "RdmNode.h"

// Integration layer between packets and rdm::Node, shared by the companion (MyMesh) and the simulator.
// NodeHost mesh side (sending, lookupContact via ContactInfo::isFav, millis, random32, txIdle, selfPub) is implemented here;
// the app side (pushUserStatus, pushSendConfirmed, pushMsgWaiting, rtcNow) stays pure virtual.
class RdmChatMesh : public BaseChatMesh, protected rdm::NodeHost {
public:
  using BaseChatMesh::sendAnonReq;   // the NodeHost override of the same name would hide it

protected:
  RdmChatMesh(mesh::Radio& radio, mesh::MillisecondClock& ms, mesh::RNG& rng, mesh::RTCClock& rtc,
              mesh::PacketManager& mgr, mesh::MeshTables& tables, rdm::FileIO& io);
  rdm::Node& rdm();

  // Mesh/BaseChatMesh overrides
  int  searchPeersByHash(const uint8_t* hash) override;            // base + hidden peers appended
  void getPeerSharedSecret(uint8_t* dest_secret, int peer_idx) override;
  void onPeerDataRecv(mesh::Packet* pkt, uint8_t type, int sender_idx, const uint8_t* secret, uint8_t* data, size_t len) override;
  bool onPeerPathRecv(mesh::Packet* pkt, int sender_idx, const uint8_t* secret, uint8_t* path, uint8_t path_len,
                      uint8_t extra_type, uint8_t* extra, uint8_t extra_len) override;
  bool onContactPathRecv(ContactInfo& from, uint8_t* in_path, uint8_t in_path_len, uint8_t* out_path, uint8_t out_path_len,
                         uint8_t extra_type, uint8_t* extra, uint8_t extra_len) override;
  void onAckRecv(mesh::Packet* pkt, uint32_t ack_crc) override;
  void onAdvertRecv(mesh::Packet* pkt, const mesh::Identity& id, uint32_t ts, const uint8_t* app_data, size_t len) override;
  // K3: a contact was removed because it is a hidden peer; the app side informs the app and persists contacts
  virtual void rdmOnContactRemoved(const ContactInfo& removed) {}

  // NodeHost, mesh side
  uint32_t millis() override;
  uint32_t random32() override;
  bool     txIdle() override;
  void     selfPub(uint8_t pub_out[32]) override;
  bool lookupContact(const uint8_t pub_prefix[6], uint8_t pub_out[32], bool& favourite, bool& has_path) override;
  bool sendTxtPlain(const uint8_t pub_prefix[6], const uint8_t* plain, size_t len, bool flood, uint32_t& est_timeout_ms) override;
  bool sendAck(const uint8_t pub_prefix[6], const uint8_t* ack, uint8_t len) override;
  bool sendReq(const uint8_t peer_pub[32], uint32_t req_ts, const uint8_t* body, size_t len, bool flood,
               uint32_t& est_timeout_ms) override;
  bool sendAnonReq(const uint8_t peer_pub[32], const uint8_t* plain, size_t len, uint32_t& est_timeout_ms) override;
  bool encryptTxtPayload(const uint8_t pub_prefix[6], const uint8_t* plain, size_t len,
                         uint8_t* payload_out, size_t& payload_len) override;
  bool decryptTxtPayload(const uint8_t* payload, size_t len, uint8_t sender_pub_out[32],
                         uint8_t* plain_out, size_t& plain_len) override;
  void onHiddenPeersChanged() override;   // K3: mailboxes leave contacts[]

private:
  rdm::Node _rdm;

  struct HiddenPeer {               // mailbox: not in contacts[], so path and secret live here
    bool    used;
    uint8_t pub[32];
    uint8_t secret[PUB_KEY_SIZE];
    uint8_t out_path_len;           // OUT_PATH_UNKNOWN: flood
    uint8_t out_path[MAX_PATH_SIZE];
  };
  HiddenPeer _hp[RDM_HIDDEN_PEERS];
  uint8_t    _hp_next = 0;
  int        _n_base_match = 0;     // contacts matched by the latest searchPeersByHash
  uint8_t    _hidden_match[RDM_HIDDEN_PEERS];

  HiddenPeer&    hiddenPeer(const uint8_t pub[32]);
  const uint8_t* matchedHiddenPub(int peer_idx) const;
  void handleTxtPlain(mesh::Packet* pkt, ContactInfo& from, const uint8_t* secret, uint8_t* data, size_t len);
  void sendReply(mesh::Packet* pkt, ContactInfo& from, const uint8_t* secret, const uint8_t* reply, uint8_t len);
  void sendToPeer(mesh::Packet* pkt, const uint8_t peer_pub[32], bool flood, uint32_t& est_timeout_ms);
};
