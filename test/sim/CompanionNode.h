#pragma once

// The real companion firmware (examples/companion_radio: MyMesh + DataStore on the node's flash) plus an
// emulated phone app that speaks the companion protocol over a simulated BLE link.

#include "ChatNode.h"

#include <FS.h>
#include <MyMesh.h>
#ifdef WITH_RELIABLE_DM
  #include <helpers/rdm/arduino/ArduinoFileIO.h>
#endif

#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace sim {

class SimSerialLink : public BaseSerialInterface {
  bool _enabled = false;
  bool _connected = false;
public:
  std::deque<std::vector<uint8_t>> to_radio, to_app;

  void enable() override { _enabled = true; }
  void disable() override { _enabled = false; }
  bool isEnabled() const override { return _enabled; }
  bool isConnected() const override { return _connected; }
  bool isWriteBusy() const override { return false; }
  size_t writeFrame(const uint8_t src[], size_t len) override;
  size_t checkRecvFrame(uint8_t dest[]) override;

  void setConnected(bool c);
};

class CompanionNode;

// Behaves like a phone app: one command in flight at a time, syncs on PUSH_CODE_MSG_WAITING, matches
// PUSH_CODE_SEND_CONFIRMED against RESP_CODE_SENT.expected_ack, and marks a message failed after est_timeout.
class CompanionApp {
public:
  using Status = ChatNode::Status;

  struct Msg {
    int from_index;
    std::string text;
    uint32_t sender_timestamp;
    uint8_t txt_type;
    uint8_t path_len;
    uint64_t at_ms;
  };

  struct Sent {
    int id;
    int to_index;
    std::string text;
    uint32_t timestamp;
    uint8_t attempt;
    bool answered;  // RESP_CODE_SENT or ERR received
    bool flood;
    uint32_t expected_ack;
    uint32_t est_timeout_ms;
    Status status;
    bool timed_out_before_ack;
    uint64_t sent_at_ms;
    uint64_t acked_at_ms;
  };

  explicit CompanionApp(CompanionNode& node) : _node(node) {}

  bool connected() const { return _connected; }
  // BLE connect + CMD_DEVICE_QUERY(v3) + CMD_APP_START + CMD_SET_DEVICE_TIME (phone clock = sim wall clock).
  void connect();
  void disconnect();
  bool auto_sync = true;

  int send_text(const SimNode& to, const std::string& text, uint8_t attempt = 0, uint32_t timestamp = 0);
  int send_text_with_retries(const SimNode& to, const std::string& text, AppRetry policy = AppRetry());
  void sync();
  void advert(bool flood = true);
  void reset_path(const SimNode& to);
  // Any other companion command (e.g. fork extensions): queued like the rest, `on_response` gets every reply frame.
  // A multi-frame reply keeps the command in flight until its closing frame: `closing_code` names it (a fork list
  // gives its END code); by default the first reply closes, except CMD_GET_CONTACTS (RESP_CODE_END_OF_CONTACTS).
  // An error frame always closes.
  using ResponseFn = std::function<void(const std::vector<uint8_t>& frame)>;
  static constexpr uint8_t FIRST_REPLY = 0xFF;
  void send_command(std::vector<uint8_t> frame, ResponseFn on_response = nullptr, uint8_t closing_code = FIRST_REPLY);

  bool idle() const { return !_in_flight && _pending.empty(); }
  uint64_t nextDeadlineMs() const;
  const std::vector<Msg>& inbox() const { return _inbox; }
  size_t inbox_count(const std::string& text) const { return countText(_inbox, text); }
  const Sent& sent(int id) const { return _sent.at(id); }
  Status retry_status(int retry_id) const;
  bool retry_done(int retry_id) const { return _retries.done(retry_id); }
  const std::vector<std::vector<uint8_t>>& push_log() const { return _pushes; }
  size_t push_count(uint8_t code) const;

  void onRadioBoot();  // the BLE link drops when the radio resets
  void tick();

private:
  struct Cmd {
    std::vector<uint8_t> frame;
    int sent_id;  // for CMD_SEND_TXT_MSG
    ResponseFn on_response;
    uint8_t closing_code;
  };

  CompanionNode& _node;
  bool _connected = false;
  std::deque<Cmd> _pending;
  std::unique_ptr<Cmd> _in_flight;
  bool _sync_queued = false;
  std::vector<Msg> _inbox;
  std::vector<Sent> _sent;
  RetryTracker _retries;
  std::vector<std::vector<uint8_t>> _pushes;

  void enqueue(std::vector<uint8_t> frame, int sent_id = -1);
  bool closes(const Cmd& cmd, const std::vector<uint8_t>& f) const;
  void handleResponse(const Cmd& cmd, const std::vector<uint8_t>& f);
  void handlePush(const std::vector<uint8_t>& f);
  void handleContactMsg(const std::vector<uint8_t>& f);
};

class CompanionNode : public SimNode {
public:
  CompanionNode(Simulator& sim, std::string name, int index);
  ~CompanionNode() override;

  CompanionApp& app() { return _app; }
  MyMesh& firmware() { return *_firmware; }
  SimSerialLink& link() { return _link; }
  // H1: the firmware's own "clock synced since boot" flag (MyMesh::rtcTrusted() with WITH_RELIABLE_DM);
  // firmware without it falls back to the simulator's view (SimRTC::setSinceBoot()).
  bool rtcTrusted();

protected:
  mesh::Mesh* createMesh() override;
  void beginMesh() override;
  void loopMesh() override;
  void onLoop() override;
  void onShutdown() override;
  bool busy() const override;
  uint64_t nextWakeupMs() const override;

private:
  fs::BoundFS _flash;
#ifdef WITH_RELIABLE_DM
  std::unique_ptr<rdm::ArduinoFileIO> _rdm_io;   // stateless, on the node's flash like main.cpp: a new one per boot
#endif
  std::unique_ptr<DataStore> _store;
  MyMesh* _firmware = nullptr;
  SimSerialLink _link;
  CompanionApp _app;
};

}
