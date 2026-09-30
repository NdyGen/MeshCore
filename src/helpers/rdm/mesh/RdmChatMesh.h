#pragma once

#include <helpers/BaseChatMesh.h>
#include <helpers/rdm/RdmNode.h>

// Integration layer between packets and rdm::Node, shared by the companion (MyMesh) and the simulator.
// The mesh side of the node (sending, lookupContact via ContactInfo::isFav, millis, random32, txIdle, selfPub) lives
// here behind a private adapter; the app side (rdm::NodeAppSink) is the derived class's, passed to the constructor.
class RdmChatMesh : public BaseChatMesh {
protected:
  RdmChatMesh(mesh::Radio& radio, mesh::MillisecondClock& ms, mesh::RNG& rng, mesh::RTCClock& rtc,
              mesh::PacketManager& mgr, mesh::MeshTables& tables, rdm::FileIO& io, rdm::NodeAppSink& app);
  rdm::Node& rdm();

  // CMD_SEND_TXT_MSG: the outbox decides, the DM goes out with the cap trailer. handled = false: not an outbox
  // message, send it the upstream way. The app gets RESP_CODE_SENT with app_ack and est_timeout_ms.
  struct AppSend { bool handled; bool flood; uint32_t app_ack; uint32_t est_timeout_ms; };
  AppSend rdmSendApp(const ContactInfo& to, uint32_t ts, uint8_t attempt, const char* text, size_t len);
  // CMD_SYNC_NEXT_MESSAGE: every request confirms the record handed out before it (03 par. 1c); false: inbox empty
  bool    rdmNextInbox(rdm::InRecord& out);

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

private:
  // What rdm::Node sees of the mesh. RdmChatMesh itself is no host: BaseChatMesh::sendAnonReq stays visible to the
  // derived class, and the host names are not part of its interface.
  class MeshHost : public rdm::NodeMeshHost {
    RdmChatMesh& _m;
  public:
    explicit MeshHost(RdmChatMesh& m) : _m(m) {}
    uint32_t millis() override { return _m.millis(); }
    uint32_t random32() override { return _m.random32(); }
    bool     txIdle() override { return _m.txIdle(); }
    void     selfPub(uint8_t pub_out[32]) override { _m.selfPub(pub_out); }
    bool lookupContact(const uint8_t pub_prefix[6], uint8_t pub_out[32], bool& favourite, bool& has_path) override {
      return _m.lookupContact(pub_prefix, pub_out, favourite, has_path);
    }
    bool sendTxtPlain(const uint8_t pub_prefix[6], const uint8_t* plain, size_t len, bool flood, uint32_t& est_timeout_ms) override {
      return _m.sendTxtPlain(pub_prefix, plain, len, flood, est_timeout_ms);
    }
    bool sendAck(const uint8_t pub_prefix[6], const uint8_t* ack, uint8_t len) override { return _m.sendAck(pub_prefix, ack, len); }
    bool sendReq(const uint8_t peer_pub[32], uint32_t req_ts, const uint8_t* body, size_t len, bool flood,
                 uint32_t& est_timeout_ms) override {
      return _m.sendReq(peer_pub, req_ts, body, len, flood, est_timeout_ms);
    }
    bool sendAnonReq(const uint8_t peer_pub[32], const uint8_t* plain, size_t len, uint32_t& est_timeout_ms) override {
      return _m.sendAnonReqToPeer(peer_pub, plain, len, est_timeout_ms);
    }
    bool encryptTxtPayload(const uint8_t pub_prefix[6], const uint8_t* plain, size_t len,
                           uint8_t* payload_out, size_t& payload_len) override {
      return _m.encryptTxtPayload(pub_prefix, plain, len, payload_out, payload_len);
    }
    bool decryptTxtPayload(const uint8_t* payload, size_t len, uint8_t sender_pub_out[32],
                           uint8_t* plain_out, size_t& plain_len) override {
      return _m.decryptTxtPayload(payload, len, sender_pub_out, plain_out, plain_len);
    }
    void onHiddenPeersChanged() override { _m.onHiddenPeersChanged(); }
  };

  MeshHost  _host;
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
  uint32_t estTimeoutForPlain(size_t plain_len, const ContactInfo& to) const;

  // mesh side of the node (see MeshHost)
  uint32_t millis();
  uint32_t random32();
  bool     txIdle();
  void     selfPub(uint8_t pub_out[32]);
  bool lookupContact(const uint8_t pub_prefix[6], uint8_t pub_out[32], bool& favourite, bool& has_path);
  bool sendTxtPlain(const uint8_t pub_prefix[6], const uint8_t* plain, size_t len, bool flood, uint32_t& est_timeout_ms);
  bool sendAck(const uint8_t pub_prefix[6], const uint8_t* ack, uint8_t len);
  bool sendReq(const uint8_t peer_pub[32], uint32_t req_ts, const uint8_t* body, size_t len, bool flood,
               uint32_t& est_timeout_ms);
  bool sendAnonReqToPeer(const uint8_t peer_pub[32], const uint8_t* plain, size_t len, uint32_t& est_timeout_ms);
  bool encryptTxtPayload(const uint8_t pub_prefix[6], const uint8_t* plain, size_t len,
                         uint8_t* payload_out, size_t& payload_len);
  bool decryptTxtPayload(const uint8_t* payload, size_t len, uint8_t sender_pub_out[32],
                         uint8_t* plain_out, size_t& plain_len);
  void onHiddenPeersChanged();   // K3: mailboxes leave contacts[]
};
