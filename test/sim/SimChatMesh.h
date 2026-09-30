#pragma once

// What every simulated chat client shares on top of BaseChatMesh: the companion's send timeouts (so est_timeout
// matches what the app receives), empty stubs for the callbacks a test client does not observe, and the flash
// layout of its contacts.

#include "Sim.h"

#include <helpers/BaseChatMesh.h>

namespace sim {

// Same constants as examples/companion_radio/MyMesh.cpp.
constexpr uint32_t SEND_TIMEOUT_BASE_MILLIS = 500;
constexpr float FLOOD_SEND_TIMEOUT_FACTOR = 16.0f;
constexpr float DIRECT_SEND_PERHOP_FACTOR = 6.0f;
constexpr uint32_t DIRECT_SEND_PERHOP_EXTRA_MILLIS = 250;

inline uint32_t floodTimeoutMs(uint32_t airtime) {
  return SEND_TIMEOUT_BASE_MILLIS + (FLOOD_SEND_TIMEOUT_FACTOR * airtime);
}

inline uint32_t directTimeoutMs(uint32_t airtime, uint8_t path_len) {
  uint8_t hops = path_len & 63;
  return SEND_TIMEOUT_BASE_MILLIS + ((airtime * DIRECT_SEND_PERHOP_FACTOR + DIRECT_SEND_PERHOP_EXTRA_MILLIS) * (hops + 1));
}

class SimChatMeshBase : public DeterministicPeerData<BaseChatMesh> {
public:
  SimChatMeshBase(mesh::Radio& radio, mesh::MillisecondClock& ms, mesh::RNG& rng, mesh::RTCClock& rtc,
                  mesh::PacketManager& mgr, mesh::MeshTables& tables)
      : DeterministicPeerData<BaseChatMesh>(radio, ms, rng, rtc, mgr, tables) {}

protected:
  void onDiscoveredContact(ContactInfo&, bool, uint8_t, const uint8_t*) override {}
  ContactInfo* processAck(const uint8_t*) override { return nullptr; }
  void onContactPathUpdated(const ContactInfo&) override {}
  void onMessageRecv(const ContactInfo&, mesh::Packet*, uint32_t, const char*) override {}
  void onCommandDataRecv(const ContactInfo&, mesh::Packet*, uint32_t, const char*) override {}
  void onCLICommandRecv(const ContactInfo&, mesh::Packet*, uint32_t, const char*, char*) override {}
  void onSignedMessageRecv(const ContactInfo&, mesh::Packet*, uint32_t, const uint8_t*, const char*) override {}
  uint32_t calcFloodTimeoutMillisFor(uint32_t airtime) const override { return floodTimeoutMs(airtime); }
  uint32_t calcDirectTimeoutMillisFor(uint32_t airtime, uint8_t path_len) const override {
    return directTimeoutMs(airtime, path_len);
  }
  void onSendTimeout() override {}  // the companion does nothing here either; timeouts are tracked per message
  void onChannelMessageRecv(const mesh::GroupChannel&, mesh::Packet*, uint32_t, const char*) override {}
  uint8_t onContactRequest(const ContactInfo&, uint32_t, const uint8_t*, uint8_t, uint8_t*) override { return 0; }
  void onContactResponse(const ContactInfo&, const uint8_t*, uint8_t) override {}
};

// Flash layout of a sim client's contacts: consecutive raw ContactInfo records (the shared-secret cache is
// recomputed on load).
struct SimContacts {
  static constexpr const char* FILE = "/contacts";

  static void save(BaseChatMesh& chat, SimFS& fs) {
    std::vector<uint8_t> blob;
    ContactInfo c;
    for (int i = 0; i < chat.getTotalContactSlots(); i++) {
      if (!chat.getContactByIdx(i, c) || c.type == ADV_TYPE_NONE) continue;
      const uint8_t* p = reinterpret_cast<const uint8_t*>(&c);
      blob.insert(blob.end(), p, p + sizeof(c));
    }
    fs.files[FILE] = blob;
  }

  static void load(BaseChatMesh& chat, SimFS& fs) {
    auto it = fs.files.find(FILE);
    if (it == fs.files.end()) return;
    const std::vector<uint8_t>& blob = it->second;
    for (size_t off = 0; off + sizeof(ContactInfo) <= blob.size(); off += sizeof(ContactInfo)) {
      ContactInfo c;
      memcpy(reinterpret_cast<uint8_t*>(&c), &blob[off], sizeof(c));
      chat.addContact(c);
    }
  }
};

}
