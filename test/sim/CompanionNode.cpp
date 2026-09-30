#include "CompanionNode.h"

#include <algorithm>
#include <cstring>
#include <type_traits>

// Companion protocol codes (examples/companion_radio/MyMesh.cpp:8-133); they are file-local there.
namespace {
constexpr uint8_t CMD_APP_START = 1;
constexpr uint8_t CMD_SEND_TXT_MSG = 2;
constexpr uint8_t CMD_SET_DEVICE_TIME = 6;
constexpr uint8_t CMD_SEND_SELF_ADVERT = 7;
constexpr uint8_t CMD_SYNC_NEXT_MESSAGE = 10;
constexpr uint8_t CMD_RESET_PATH = 13;
constexpr uint8_t CMD_DEVICE_QUERY = 22;
constexpr uint8_t CMD_GET_CONTACTS = 4;
constexpr uint8_t CMD_RDM_LIST_OUTBOX = 0xC2;       // fork (06 par. 3.14)

constexpr uint8_t RESP_CODE_ERR = 1;
constexpr uint8_t RESP_CODE_END_OF_CONTACTS = 4;
constexpr uint8_t RESP_CODE_RDM_OUTBOX_END = 0x42;  // fork (06 par. 3.14)
constexpr uint8_t RESP_CODE_SENT = 6;
constexpr uint8_t RESP_CODE_CONTACT_MSG_RECV = 7;
constexpr uint8_t RESP_CODE_NO_MORE_MESSAGES = 10;
constexpr uint8_t RESP_CODE_CONTACT_MSG_RECV_V3 = 16;

constexpr uint8_t PUSH_CODE_SEND_CONFIRMED = 0x82;
constexpr uint8_t PUSH_CODE_MSG_WAITING = 0x83;

constexpr uint8_t APP_PROTOCOL_VER = 3;
}

namespace sim {

// ---------------------------------------------------------------- SimSerialLink

size_t SimSerialLink::writeFrame(const uint8_t src[], size_t len) {
  if (!_connected) return 0;
  to_app.emplace_back(src, src + len);
  return len;
}

size_t SimSerialLink::checkRecvFrame(uint8_t dest[]) {
  if (!_connected || to_radio.empty()) return 0;
  std::vector<uint8_t> f = std::move(to_radio.front());
  to_radio.pop_front();
  memcpy(dest, f.data(), f.size());
  return f.size();
}

void SimSerialLink::setConnected(bool c) {
  _connected = c;
  to_radio.clear();
  to_app.clear();
}

// ---------------------------------------------------------------- CompanionApp

void CompanionApp::enqueue(std::vector<uint8_t> frame, int sent_id) { _pending.push_back(Cmd{std::move(frame), sent_id, nullptr}); }

void CompanionApp::send_command(std::vector<uint8_t> frame, ResponseFn on_response) {
  if (_connected) _pending.push_back(Cmd{std::move(frame), -1, std::move(on_response)});
}

void CompanionApp::connect() {
  if (!_node.powered() || _connected) return;
  _connected = true;
  _node.link().setConnected(true);
  enqueue({CMD_DEVICE_QUERY, APP_PROTOCOL_VER});
  std::vector<uint8_t> start = {CMD_APP_START, 0, 0, 0, 0, 0, 0, 0};
  const char* app_name = "simapp";
  start.insert(start.end(), app_name, app_name + strlen(app_name));
  enqueue(start);
  uint32_t now = _node.sim().wall_epoch();
  std::vector<uint8_t> t = {CMD_SET_DEVICE_TIME, 0, 0, 0, 0};
  memcpy(&t[1], &now, 4);
  enqueue(t);
  if (auto_sync) sync();
}

void CompanionApp::disconnect() {
  _connected = false;
  _pending.clear();
  _in_flight.reset();
  _sync_queued = false;
  if (_node.powered()) _node.link().setConnected(false);
}

void CompanionApp::onRadioBoot() {
  _connected = false;
  _pending.clear();
  _in_flight.reset();
  _sync_queued = false;
  _node.link().setConnected(false);
}

int CompanionApp::send_text(const SimNode& to, const std::string& text, uint8_t attempt, uint32_t timestamp) {
  if (!_connected) return -1;
  Sent s{};
  s.id = (int)_sent.size();
  s.to_index = to.index();
  s.text = text;
  s.timestamp = timestamp ? timestamp : _node.sim().wall_epoch();
  s.attempt = attempt;
  s.status = Status::PENDING;
  _sent.push_back(s);

  std::vector<uint8_t> f = {CMD_SEND_TXT_MSG, TXT_TYPE_PLAIN, attempt, 0, 0, 0, 0};
  memcpy(&f[3], &s.timestamp, 4);
  f.insert(f.end(), to.identity().pub_key, to.identity().pub_key + 6);
  f.insert(f.end(), text.begin(), text.end());
  enqueue(f, s.id);
  return s.id;
}

int CompanionApp::send_text_with_retries(const SimNode& to, const std::string& text, AppRetry policy) {
  int first = send_text(to, text, 0);
  if (first < 0) return -1;
  _retries.push_back(RetryJob{to.index(), text, _sent[first].timestamp, policy, {first}, false});
  return (int)_retries.size() - 1;
}

void CompanionApp::sync() {
  if (!_connected || _sync_queued) return;
  _sync_queued = true;
  enqueue({CMD_SYNC_NEXT_MESSAGE});
}

void CompanionApp::advert(bool flood) {
  if (_connected) enqueue({CMD_SEND_SELF_ADVERT, (uint8_t)(flood ? 1 : 0)});
}

void CompanionApp::reset_path(const SimNode& to) {
  if (!_connected) return;
  std::vector<uint8_t> f = {CMD_RESET_PATH};
  f.insert(f.end(), to.identity().pub_key, to.identity().pub_key + PUB_KEY_SIZE);
  enqueue(f);
}

int CompanionApp::indexForPrefix(const uint8_t* prefix, int len) const {
  Simulator& s = _node.sim();
  for (size_t i = 0; i < s.node_count(); i++) {
    if (memcmp(s.node(i).identity().pub_key, prefix, len) == 0) return (int)i;
  }
  return -1;
}

void CompanionApp::handleContactMsg(const std::vector<uint8_t>& f) {
  size_t i = 1;
  if (f[0] == RESP_CODE_CONTACT_MSG_RECV_V3) i += 3;  // snr, reserved1, reserved2
  Msg m;
  m.from_index = indexForPrefix(&f[i], 6);
  i += 6;
  m.path_len = f[i++];
  m.txt_type = f[i++];
  memcpy(&m.sender_timestamp, &f[i], 4);
  i += 4;
  if (m.txt_type == TXT_TYPE_SIGNED_PLAIN) i += 4;
  m.text.assign(f.begin() + i, f.end());
  m.at_ms = _node.sim().now();
  _inbox.push_back(m);
}

// True when `f` is the last reply frame of `cmd`.
static bool closesCommand(uint8_t cmd, const std::vector<uint8_t>& f) {
  if (f[0] == RESP_CODE_ERR) return true;
  switch (cmd) {
    case CMD_GET_CONTACTS:    return f[0] == RESP_CODE_END_OF_CONTACTS;
    case CMD_RDM_LIST_OUTBOX: return f[0] == RESP_CODE_RDM_OUTBOX_END;
    default:                  return true;
  }
}

void CompanionApp::handleResponse(const Cmd& cmd, const std::vector<uint8_t>& f) {
  if (cmd.on_response) {
    cmd.on_response(f);
    return;
  }
  switch (cmd.frame[0]) {
    case CMD_SEND_TXT_MSG: {
      Sent& s = _sent.at(cmd.sent_id);
      s.answered = true;
      s.sent_at_ms = _node.sim().now();
      if (f[0] == RESP_CODE_SENT && f.size() >= 10) {
        s.flood = f[1] == 1;
        memcpy(&s.expected_ack, &f[2], 4);
        memcpy(&s.est_timeout_ms, &f[6], 4);
      } else {
        s.status = Status::SEND_FAILED;
      }
      break;
    }
    case CMD_SYNC_NEXT_MESSAGE:
      _sync_queued = false;
      if (f[0] == RESP_CODE_CONTACT_MSG_RECV || f[0] == RESP_CODE_CONTACT_MSG_RECV_V3) {
        handleContactMsg(f);
        sync();
      } else if (f[0] != RESP_CODE_NO_MORE_MESSAGES) {
        sync();  // channel message or other frame type: skip it and keep draining
      }
      break;
    default:
      break;
  }
}

void CompanionApp::handlePush(const std::vector<uint8_t>& f) {
  _pushes.push_back(f);
  if (f[0] == PUSH_CODE_SEND_CONFIRMED && f.size() >= 5) {
    uint32_t ack;
    memcpy(&ack, &f[1], 4);
    for (auto& s : _sent) {
      if (s.answered && s.expected_ack == ack && s.acked_at_ms == 0) {
        s.status = Status::DELIVERED;
        s.acked_at_ms = _node.sim().now();
      }
    }
  } else if (f[0] == PUSH_CODE_MSG_WAITING && auto_sync) {
    sync();
  }
}

void CompanionApp::tick() {
  SimSerialLink& link = _node.link();
  while (!link.to_app.empty()) {
    std::vector<uint8_t> f = std::move(link.to_app.front());
    link.to_app.pop_front();
    if (f.empty()) continue;
    if (f[0] >= 0x80) {
      handlePush(f);
    } else if (_in_flight) {
      if (closesCommand(_in_flight->frame[0], f)) {
        std::unique_ptr<Cmd> cmd = std::move(_in_flight);
        handleResponse(*cmd, f);
      } else if (_in_flight->on_response) {
        _in_flight->on_response(f);
      }
    }
  }

  const uint64_t now = _node.sim().now();
  for (auto& s : _sent) {
    if (s.status == Status::PENDING && s.answered && now >= s.sent_at_ms + s.est_timeout_ms) {
      s.status = Status::TIMED_OUT;
      s.timed_out_before_ack = true;
    }
  }
  for (auto& job : _retries) {
    if (job.done) continue;
    const Sent& last = _sent.at(job.attempts.back());
    if (last.status == Status::DELIVERED) {
      job.done = true;
    } else if (last.status == Status::TIMED_OUT || last.status == Status::SEND_FAILED) {
      int n = (int)job.attempts.size();
      if (n >= job.policy.max_attempts || !_connected) {
        job.done = true;
        continue;
      }
      SimNode& to = _node.sim().node(job.to_index);
      if (job.policy.flood_on_last && n == job.policy.max_attempts - 1) reset_path(to);
      int id = send_text(to, job.text, (uint8_t)n, job.timestamp);
      if (id < 0) job.done = true; else job.attempts.push_back(id);
    }
  }

  if (_connected && !_in_flight && !_pending.empty()) {
    _in_flight.reset(new Cmd(std::move(_pending.front())));
    _pending.pop_front();
    link.to_radio.push_back(_in_flight->frame);
  }
}

uint64_t CompanionApp::nextDeadlineMs() const {
  uint64_t w = UINT64_MAX;
  for (const auto& s : _sent) {
    if (s.status == Status::PENDING && s.answered) w = std::min(w, s.sent_at_ms + s.est_timeout_ms);
  }
  return w;
}

size_t CompanionApp::inbox_count(const std::string& text) const {
  size_t n = 0;
  for (const auto& m : _inbox) n += m.text == text;
  return n;
}

CompanionApp::Status CompanionApp::retry_status(int retry_id) const {
  const RetryJob& job = _retries.at(retry_id);
  for (int id : job.attempts) {
    if (_sent[id].status == Status::DELIVERED) return Status::DELIVERED;
  }
  return _sent[job.attempts.back()].status;
}

size_t CompanionApp::push_count(uint8_t code) const {
  size_t n = 0;
  for (const auto& p : _pushes) n += p[0] == code;
  return n;
}

// ---------------------------------------------------------------- CompanionNode

// MyMesh gains rdm_io, rtcTrusted(), setHardwareRTC() and rdm() with WITH_RELIABLE_DM (WP6). Detecting them keeps
// the simulator building against the firmware before and after that change.
namespace {
template <class M, class = void> struct HasSetHardwareRTC : std::false_type {};
template <class M>
struct HasSetHardwareRTC<M, std::void_t<decltype(std::declval<M&>().setHardwareRTC(true))>> : std::true_type {};
template <class M, class = void> struct HasRtcTrusted : std::false_type {};
template <class M> struct HasRtcTrusted<M, std::void_t<decltype(std::declval<M&>().rtcTrusted())>> : std::true_type {};
template <class M, class = void> struct HasRdm : std::false_type {};
template <class M>
struct HasRdm<M, std::void_t<decltype(std::declval<M&>().rdm().nextWakeupMillis(0u))>> : std::true_type {};

#ifdef WITH_RELIABLE_DM
template <class M>
M* newFirmware(mesh::Radio& radio, mesh::RNG& rng, mesh::RTCClock& rtc, SimpleMeshTables& tables, DataStore& store,
               rdm::FileIO& io) {
  if constexpr (std::is_constructible<M, mesh::Radio&, mesh::RNG&, mesh::RTCClock&, SimpleMeshTables&, DataStore&,
                                      rdm::FileIO&>::value) {
    return new M(radio, rng, rtc, tables, store, io);
  } else {
    (void)io;
    return new M(radio, rng, rtc, tables, store);
  }
}
#endif

template <class M> void setHardwareRTC(M& m, bool present) {
  if constexpr (HasSetHardwareRTC<M>::value) m.setHardwareRTC(present);
}
template <class M> bool firmwareRtcTrusted(M& m, bool fallback) {
  if constexpr (HasRtcTrusted<M>::value) return m.rtcTrusted();
  else return fallback;
}
template <class M> uint32_t firmwareWakeupMillis(M& m, uint32_t millis_now) {
  if constexpr (HasRdm<M>::value) return m.rdm().nextWakeupMillis(millis_now);
  else return UINT32_MAX;
}
}

CompanionNode::CompanionNode(Simulator& sim, std::string name, int index)
    : SimNode(sim, std::move(name), index), _flash(fs()), _app(*this) {
  setSimManagesIdentity(false);
}

CompanionNode::~CompanionNode() = default;

mesh::Mesh* CompanionNode::createMesh() {
  _store.reset(new DataStore(_flash, rtc()));
#ifdef WITH_RELIABLE_DM
  _rdm_io.reset(new SimFileIO(fs()));
  _firmware = newFirmware<DeterministicPeerData<MyMesh>>(radio(), rng(), rtc(), tables(), *_store, *_rdm_io);
#else
  _firmware = new DeterministicPeerData<MyMesh>(radio(), rng(), rtc(), tables(), *_store);
#endif
  return _firmware;
}

// Same order as examples/companion_radio/main.cpp setup(): store.begin(), the_mesh.begin(), startInterface().
void CompanionNode::beginMesh() {
  _store->begin();
  setHardwareRTC(*_firmware, has_rtc_battery());
  _firmware->begin();
  _firmware->setBLEPin(0);
  _firmware->startInterface(_link);
  _app.onRadioBoot();
}

void CompanionNode::loopMesh() { _firmware->loop(); }

void CompanionNode::onLoop() { _app.tick(); }

// MyMesh owns its packet pool, so ask the firmware (outbound queue, lazy contacts write) and the app instead.
bool CompanionNode::busy() const {
  return (_firmware && _firmware->hasPendingWork()) || !_app.idle() || !_link.to_app.empty() || !_link.to_radio.empty();
}

bool CompanionNode::rtcTrusted() {
  if (!powered() || !_firmware) return false;
  return firmwareRtcTrusted(*_firmware, has_rtc_battery() || rtc().setSinceBoot());
}

uint64_t CompanionNode::nextWakeupMs() const {
  uint64_t w = _app.nextDeadlineMs();
  if (powered() && _firmware) {
    uint32_t f = firmwareWakeupMillis(*_firmware, (uint32_t)uptimeMs());
    if (f != UINT32_MAX) w = std::min(w, const_cast<CompanionNode*>(this)->sim().now() + f);
  }
  return w;
}

void CompanionNode::onShutdown() {
  _app.onRadioBoot();
  _firmware = nullptr;
}

}
