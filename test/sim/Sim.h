#pragma once

// Deterministic multi-node MeshCore simulator. Runs the real src/ protocol stack (Dispatcher, Mesh,
// BaseChatMesh, Packet, Utils, Identity + real crypto) against a virtual LoRa medium, virtual time,
// per-node RTC, RNG and flash. See docs/reliable-dm/07-simulator.md.

#include <Mesh.h>
#include <helpers/SimpleMeshTables.h>
#include <helpers/StaticPoolPacketManager.h>

#include "SimFS.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace sim {

// VolatileRTCClock's boot value (src/helpers/ArduinoHelpers.h): what a node without RTC battery reports after reboot.
constexpr uint32_t RTC_RESET_EPOCH = 1715770351;
// Sim wall clock at t=0: 2026-09-29 00:00:00 UTC.
constexpr uint32_t DEFAULT_START_EPOCH = 1790640000;

struct RadioConfig {
  uint8_t sf = 8;
  float bw_khz = 62.5f;
  uint8_t cr = 8;          // coding rate denominator, 4/cr
  uint16_t preamble = 32;  // RadioLibWrapper::preambleLengthForSF(): 32 for SF<=8
};

struct Link {
  float snr = 8.0f;
  float loss = 0.0f;  // probability [0..1] that a frame on this directed link is lost
};

struct TxRecord {
  uint64_t seq;  // event number, shared with FsWrite::seq: orders transmissions against flash writes
  uint64_t t_ms;
  int from;
  uint8_t header;
  uint8_t payload_type;
  bool flood;
  uint32_t airtime_ms;
  std::vector<uint8_t> raw;
};

struct FsWrite {
  uint64_t seq;
  uint64_t t_ms;
  int node;
  std::string path;
  uint32_t off, len;
  bool complete;
};

// Return true to drop this transmission for receiver `to_index` (counted as a loss).
using DropFilter = std::function<bool(const TxRecord& tx, int to_index)>;

class Simulator;
class SimNode;

class SimRNG : public mesh::RNG {
  uint64_t _state = 0;
public:
  void seed(uint64_t s) { _state = s; }
  uint64_t next();
  double uniform() { return (next() >> 11) * (1.0 / 9007199254740992.0); }
  void random(uint8_t* dest, size_t sz) override;
};

class SimMillis : public mesh::MillisecondClock {
  const SimNode& _node;
public:
  explicit SimMillis(const SimNode& node) : _node(node) {}
  unsigned long getMillis() override;
};

class SimRTC : public mesh::RTCClock {
  const Simulator& _sim;
  uint32_t _base_epoch;
  uint64_t _base_ms;
public:
  SimRTC(const Simulator& sim, uint32_t epoch);
  uint32_t getCurrentTime() override;
  void setCurrentTime(uint32_t time) override;
  // True once the time was set after boot (by the app or sync_clock): what firmware may treat as trustworthy.
  bool setSinceBoot() const { return _set_since_boot; }
  // For firmware that sets the clock itself at boot from stored data (bootstrapRTCfromContacts): not a sync.
  void markBootstrapped() { _set_since_boot = false; }

private:
  bool _set_since_boot = false;
};


class SimRadio : public mesh::Radio {
  Simulator& _sim;
  SimNode& _node;
  struct Frame { std::vector<uint8_t> bytes; float snr; };
  std::vector<Frame> _rx_ready;
  uint64_t _tx_end = 0;
  bool _transmitting = false;
  float _last_snr = 0, _last_rssi = 0;
public:
  SimRadio(Simulator& sim, SimNode& node) : _sim(sim), _node(node) {}

  void deliver(const std::vector<uint8_t>& bytes, float snr) { _rx_ready.push_back({bytes, snr}); }
  bool isTransmitting() const { return _transmitting; }
  bool hasPendingRx() const { return !_rx_ready.empty(); }

  int recvRaw(uint8_t* bytes, int sz) override;
  uint32_t getEstAirtimeFor(int len_bytes) override;
  float packetScore(float snr, int packet_len) override;
  bool startSendRaw(const uint8_t* bytes, int len) override;
  bool isSendComplete() override;
  void onSendFinished() override { _transmitting = false; }
  bool isInRecvMode() const override { return !_transmitting; }
  bool isReceiving() override;
  float getLastRSSI() const override { return _last_rssi; }
  float getLastSNR() const override { return _last_snr; }
};

// One simulated device. RAM state (mesh object, packet pool, dedup tables, radio, RTC object) is rebuilt on
// every boot; flash (SimFS) and the identity stored in it survive unless wiped.
class SimNode {
  friend class Simulator;

  Simulator& _sim;
  std::string _name;
  int _index;
  bool _powered = false;
  bool _rtc_battery = false;
  bool _sim_identity = true;
  bool _pending_reboot = false, _pending_power_off = false;
  uint32_t _boot_count = 0;
  uint32_t _power_epoch = 0;   // bumped on every power transition, invalidates in-flight receptions
  uint64_t _boot_ms = 0;
  uint32_t _rtc_epoch_at_off = 0;
  uint64_t _off_at_ms = 0;
  SimFS _fs;
  SimRNG _rng;

  std::unique_ptr<SimMillis> _ms;
  std::unique_ptr<SimRTC> _rtc;
  std::unique_ptr<SimRadio> _radio;
  std::unique_ptr<StaticPoolPacketManager> _mgr;
  std::unique_ptr<SimpleMeshTables> _tables;
  std::unique_ptr<mesh::Mesh> _mesh;
  mesh::LocalIdentity _identity;

  void boot();
  void shutdown();
  void loadOrCreateIdentity();

protected:
  int pool_size = 16;  // companion: StaticPoolPacketManager(16)
  // false: the firmware loads/creates its own identity in flash during begin() (companion); the sim reads it back.
  void setSimManagesIdentity(bool v) { _sim_identity = v; }

  SimNode(Simulator& sim, std::string name, int index);

  // Construct the node's mesh on top of radio()/millis()/rng()/rtc()/packetManager()/tables(). Called on every boot.
  virtual mesh::Mesh* createMesh() = 0;
  // Mesh::begin() is not virtual; firmwares with their own begin() (companion MyMesh) override this.
  virtual void beginMesh() { _mesh->begin(); }
  virtual void onBoot() {}
  virtual void onShutdown() {}
  // BaseChatMesh::loop() hides Mesh::loop() (non-virtual), so subclasses call the right one.
  virtual void loopMesh() { _mesh->loop(); }
  virtual void onLoop() {}
  // Fast-forward support: true while the node has queued/delayed work the simulator must step through in 1 ms
  // ticks. Default: some packet of the sim-owned pool is not free (queued outbound/inbound or held).
  virtual bool busy() const;
  // Absolute sim time (ms) of the node's next self-timed action, or UINT64_MAX. Lets fast-forward land exactly on
  // timers (retries, schedules) instead of up to one coarse step late.
  virtual uint64_t nextWakeupMs() const { return UINT64_MAX; }

  mesh::MillisecondClock& millisClock() { return *_ms; }
  mesh::PacketManager& packetManager() { return *_mgr; }
  SimpleMeshTables& tables() { return *_tables; }

public:
  virtual ~SimNode();
  SimNode(const SimNode&) = delete;
  SimNode& operator=(const SimNode&) = delete;

  Simulator& sim() { return _sim; }
  const std::string& name() const { return _name; }
  int index() const { return _index; }
  bool powered() const { return _powered; }
  uint32_t bootCount() const { return _boot_count; }
  uint32_t powerEpoch() const { return _power_epoch; }
  uint64_t uptimeMs() const;

  mesh::Mesh& mesh() { return *_mesh; }
  SimRadio& radio() { return *_radio; }
  SimRTC& rtc() { return *_rtc; }
  SimRNG& rng() { return _rng; }
  SimFS& fs() { return _fs; }
  const mesh::LocalIdentity& identity() const { return _identity; }

  // Power loss: RAM gone, flash kept. power_on() boots again.
  void power_off();
  void power_on();
  // Abrupt reset. wipe_fs = factory reset (new identity, no contacts).
  void reboot(bool wipe_fs = false);
  // With an RTC battery the clock keeps running while off; without one it restarts at RTC_RESET_EPOCH (2024).
  void set_rtc_battery(bool present) { _rtc_battery = present; }
  bool has_rtc_battery() const { return _rtc_battery; }
  // What a connected app does: push the phone's (= sim wall clock) time to the node.
  void sync_clock();
  // For firmware calling board.reboot()/powerOff() from inside its own loop: applied after that loop returns.
  void request_reboot() { _pending_reboot = true; }
  void request_power_off() { _pending_power_off = true; }

  void loop();
};

class Simulator {
  friend class SimRadio;
  friend class SimNode;

  struct Reception {
    int to, from;
    std::vector<uint8_t> bytes;
    uint64_t start, end;
    float snr;
    float loss;
    int corrupted;  // DropReason
    uint32_t to_power_epoch;
  };

  uint64_t _seed;
  uint64_t _now = 0;
  uint32_t _tick = 1;
  uint32_t _start_epoch = DEFAULT_START_EPOCH;
  bool _collisions = true;
  bool _trace = false;
  SimRNG _rng;
  std::vector<std::unique_ptr<SimNode>> _nodes;
  std::map<std::pair<int, int>, Link> _links;
  std::vector<Reception> _in_flight;
  std::vector<TxRecord> _tx_log;
  std::vector<FsWrite> _fs_log;
  uint64_t _event_seq = 0;
  SimNode* _current = nullptr;
  uint64_t _dropped_collision = 0, _dropped_loss = 0, _dropped_halfduplex = 0, _dropped_off = 0, _dropped_filter = 0;
  std::vector<DropFilter> _drop_filters;
  uint32_t _ff_max_step = 0;
  uint32_t _ff_settle = 40000;
  uint64_t _last_activity = 0;

  bool idle() const;
  uint64_t nextStep() const;

  void transmit(SimNode& from, const uint8_t* bytes, int len);
  void deliverDue();
  bool receptionActiveAt(int node_index) const;
  void step(uint64_t limit);
  void traceTx(const SimNode& from, const uint8_t* bytes, int len);

public:
  explicit Simulator(uint64_t seed = 1, RadioConfig cfg = RadioConfig());
  ~Simulator();

  RadioConfig radio_cfg;

  template <class T, class... Args>
  T& add(const std::string& name, Args&&... args) {
    auto node = std::unique_ptr<T>(new T(*this, name, (int)_nodes.size(), std::forward<Args>(args)...));
    T& ref = *node;
    _nodes.push_back(std::move(node));
    ref.boot();
    return ref;
  }

  // Bidirectional link with the same quality both ways.
  void link(SimNode& a, SimNode& b, float snr = 8.0f, float loss = 0.0f);
  void link_oneway(SimNode& from, SimNode& to, float snr = 8.0f, float loss = 0.0f);
  void unlink(SimNode& a, SimNode& b);
  bool linked(const SimNode& from, const SimNode& to) const;

  void set_collisions(bool enabled) { _collisions = enabled; }
  void set_tick(uint32_t ms) { _tick = ms ? ms : 1; }
  void set_trace(bool enabled) { _trace = enabled; }
  // Fast-forward: when the air is quiet for `settle_ms`, no node is busy() and no frame is pending, advance time
  // in steps of up to `max_step_ms` (bounded by every node's nextWakeupMs()). 0 disables (default: 1 ms ticks).
  // settle_ms covers delays the sim cannot see (flood rx delay up to 32 s, retransmit jitter).
  void set_fast_forward(uint32_t max_step_ms, uint32_t settle_ms = 40000) { _ff_max_step = max_step_ms; _ff_settle = settle_ms; }

  // Targeted loss (e.g. "lose the next ACK from alice"): every filter is asked per transmission and receiver.
  void add_drop_filter(DropFilter f) { _drop_filters.push_back(std::move(f)); }
  void clear_drop_filters() { _drop_filters.clear(); }
  // Drops the next `count` transmissions of `payload_type` (from/to -1 = any), then stops dropping.
  void drop_next(uint8_t payload_type, int from_index = -1, int to_index = -1, int count = 1);

  void run_for(uint64_t ms);
  // Runs until pred() is true (checked every tick) or timeout; returns pred().
  bool run_until(const std::function<bool()>& pred, uint64_t timeout_ms);

  uint64_t now() const { return _now; }
  uint32_t wall_epoch() const { return _start_epoch + (uint32_t)(_now / 1000); }
  uint32_t airtime_ms(int len_bytes) const;

  SimNode* current() const { return _current; }
  // Code driven from a test (outside the node's loop) must run "on" that node so millis()/random() resolve to it.
  class OnNode {
    Simulator& _s;
    SimNode* _prev;
  public:
    OnNode(Simulator& s, SimNode& n) : _s(s), _prev(s._current) { s._current = &n; }
    ~OnNode() { _s._current = _prev; }
  };

  size_t node_count() const { return _nodes.size(); }
  SimNode& node(size_t i) { return *_nodes.at(i); }
  SimRNG& rng() { return _rng; }
  const std::vector<TxRecord>& tx_log() const { return _tx_log; }
  const std::vector<FsWrite>& fs_log() const { return _fs_log; }
  void log_fs_write(int node, const std::string& path, uint32_t off, uint32_t len, bool complete);
  size_t count_tx(int from_index = -1, int payload_type = -1) const;

  uint64_t dropped_collision() const { return _dropped_collision; }
  uint64_t dropped_loss() const { return _dropped_loss; }
  uint64_t dropped_halfduplex() const { return _dropped_halfduplex; }
  uint64_t dropped_off() const { return _dropped_off; }
  uint64_t dropped_filter() const { return _dropped_filter; }
};

// BaseChatMesh::onPeerDataRecv() (src/helpers/BaseChatMesh.cpp) reads data[len + 1] for the optional attempt
// byte behind the text's NUL. When the plaintext fills its last AES block exactly, that byte is uninitialised
// stack of Mesh::onRecvPacket() and ends up in the ACK; on real hardware it is whatever the stack held. The
// simulator pins it to 0 so two runs with one seed give identical bytes. The buffer is MAX_PACKET_PAYLOAD long
// and upstream itself writes data[len].
template <class M>
class DeterministicPeerData : public M {
public:
  using M::M;

protected:
  void onPeerDataRecv(mesh::Packet* packet, uint8_t type, int sender_idx, const uint8_t* secret, uint8_t* data,
                      size_t len) override {
    data[len + 1] = 0;
    M::onPeerDataRecv(packet, type, sender_idx, secret, data, len);
  }
};

const char* payloadTypeName(uint8_t type);

}
