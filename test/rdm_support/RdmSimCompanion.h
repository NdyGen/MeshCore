#pragma once

// A fork companion in the simulator: RdmChatMesh (the same integration layer MyMesh uses under
// WITH_RELIABLE_DM) on a SimNode, with the app side as direct calls instead of companion frames. The send and
// sync recipes here are the ones MyMesh follows for CMD_SEND_TXT_MSG and CMD_SYNC_NEXT_MESSAGE.

#include <ChatNode.h>
#include <SimFileIO.h>

#include <helpers/rdm/RdmChatMesh.h>

#include <string>
#include <vector>

namespace sim {

class RdmSimCompanion : public SimNode {
public:
  struct StatusPush { uint64_t t_ms; uint32_t app_ack; rdm::UserStatus s; uint32_t ts; uint8_t prefix[6]; };
  struct AppMsg { int from_index; std::string text; uint32_t sender_timestamp; uint64_t at_ms; bool via_rdm; };
  struct Sent { uint32_t app_ack; uint32_t ts; std::string text; int to_index; bool handled; uint64_t at_ms; };

  RdmSimCompanion(Simulator& sim, std::string name, int index);
  ~RdmSimCompanion() override;

  // phone app
  void connect(bool rdm_client = true);   // app sets the clock (CMD_SET_DEVICE_TIME), then syncs
  void disconnect();
  bool connected() const { return _connected; }
  bool auto_sync = true;                  // sync on PUSH_CODE_MSG_WAITING while connected
  int  send_text(const SimNode& to, const std::string& text, uint8_t attempt = 0, uint32_t ts = 0);
  void sync();
  const Sent& sent(int id) const { return _sent.at(id); }
  const std::vector<AppMsg>& inbox() const { return _inbox; }
  size_t inbox_count(const std::string& text) const;
  const std::vector<StatusPush>& statuses() const { return _statuses; }
  std::vector<rdm::UserStatus> statuses_for(uint32_t app_ack) const;
  const std::vector<uint32_t>& confirms() const { return _confirms; }
  // K3: pubkeys of contacts RdmChatMesh removed because they became hidden peers (mailboxes), in order
  const std::vector<std::vector<uint8_t>>& removed_contacts() const { return _removed; }

  // radio-side setup the app would do
  void advert(bool flood = true);
  void add_contact(const SimNode& other, bool favourite = true);
  bool knows(const SimNode& other);
  bool has_path_to(const SimNode& other);
  bool set_mailbox(const uint8_t* mbx_pub, const uint8_t* k_owner);
  rdm::Node& rdm();

protected:
  mesh::Mesh* createMesh() override;
  void beginMesh() override;
  void loopMesh() override;
  void onLoop() override;
  bool busy() const override;
  uint64_t nextWakeupMs() const override;

private:
  class Impl;
  friend class Impl;

  // Reports RDM writes to the simulator's fs_log (SimFileIO writes the map directly).
  class LoggingIO : public rdm::FileIO {
    SimFS& _fs;
    SimFileIO _io;
  public:
    explicit LoggingIO(SimFS& fs) : _fs(fs), _io(fs) {}
    bool exists(const char* p) override { return _io.exists(p); }
    int32_t size(const char* p) override { return _io.size(p); }
    bool create(const char* p, uint32_t n) override;
    bool read(const char* p, uint32_t off, uint8_t* b, uint32_t n) override { return _io.read(p, off, b, n); }
    bool write(const char* p, uint32_t off, const uint8_t* b, uint32_t n) override;
    bool remove(const char* p) override { return _io.remove(p); }
    uint32_t freeBytes() override { return _io.freeBytes(); }
  };

  LoggingIO _io;
  Impl* _impl = nullptr;
  bool _connected = false;
  bool _rdm_client = false;
  bool _sync_wanted = false;
  std::vector<Sent> _sent;
  std::vector<AppMsg> _inbox;
  std::vector<AppMsg> _upstream_queue;    // RDM off: upstream offline queue (RAM)
  std::vector<StatusPush> _statuses;
  std::vector<uint32_t> _confirms;
  std::vector<std::vector<uint8_t>> _removed;
  struct UpstreamAck { uint32_t ack; uint8_t pub[32]; };
  std::vector<UpstreamAck> _upstream_acks;   // expected ACKs of upstream sends (RDM off, TOO_BIG, NO_OUTBOX)

  void saveContacts();
  void loadContacts();
  int  indexForPubKey(const uint8_t* pub) const;
};

}
