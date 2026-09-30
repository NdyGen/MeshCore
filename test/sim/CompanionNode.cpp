#include "CompanionNode.h"

#include "CompanionFrames.h"

#include <algorithm>
#include <cstring>

using namespace sim::frames;

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

void CompanionApp::enqueue(std::vector<uint8_t> frame, int sent_id) {
  _pending.push_back(Cmd{std::move(frame), sent_id, nullptr, FIRST_REPLY});
}

void CompanionApp::send_command(std::vector<uint8_t> frame, ResponseFn on_response, uint8_t closing_code) {
  if (_connected) _pending.push_back(Cmd{std::move(frame), -1, std::move(on_response), closing_code});
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
  rdm::put32(&t[1], now);
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
  enqueue(buildSendTxt(to.identity().pub_key, s.timestamp, attempt, text), s.id);
  return s.id;
}

int CompanionApp::send_text_with_retries(const SimNode& to, const std::string& text, AppRetry policy) {
  int first = send_text(to, text, 0);
  if (first < 0) return -1;
  return _retries.add(to.index(), text, _sent[first].timestamp, policy, first);
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

void CompanionApp::handleContactMsg(const std::vector<uint8_t>& f) {
  ContactMsg c;
  if (!decodeContactMsg(f, c)) return;
  Msg m;
  m.from_index = _node.sim().indexOfPrefix(c.sender_prefix, 6);
  m.path_len = c.path_len;
  m.txt_type = c.txt_type;
  m.sender_timestamp = c.ts;
  m.text = c.text;
  m.at_ms = _node.sim().now();
  _inbox.push_back(m);
}

// True when `f` is the last reply frame of `cmd`.
bool CompanionApp::closes(const Cmd& cmd, const std::vector<uint8_t>& f) const {
  if (f[0] == RESP_CODE_ERR) return true;
  if (cmd.closing_code != FIRST_REPLY) return f[0] == cmd.closing_code;
  if (cmd.frame[0] == CMD_GET_CONTACTS) return f[0] == RESP_CODE_END_OF_CONTACTS;
  return true;
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
      SentReply r;
      if (decodeSent(f, r)) {
        s.flood = r.flood;
        s.expected_ack = r.expected_ack;
        s.est_timeout_ms = r.est_timeout_ms;
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
    uint32_t ack = rdm::get32(&f[1]);
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
      if (closes(*_in_flight, f)) {
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
  _retries.tick([this](int id) { return _sent.at(id).status; },
                [this](int to) { reset_path(_node.sim().node(to)); },
                [this](int to, const std::string& text, uint8_t attempt, uint32_t ts) {
                  return send_text(_node.sim().node(to), text, attempt, ts);
                });

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

CompanionApp::Status CompanionApp::retry_status(int retry_id) const {
  return _retries.status(retry_id, [this](int id) { return _sent.at(id).status; });
}

size_t CompanionApp::push_count(uint8_t code) const {
  size_t n = 0;
  for (const auto& p : _pushes) n += p[0] == code;
  return n;
}

// ---------------------------------------------------------------- CompanionNode

CompanionNode::CompanionNode(Simulator& sim, std::string name, int index)
    : SimNode(sim, std::move(name), index), _flash(fs()), _app(*this) {
  setSimManagesIdentity(false);
}

CompanionNode::~CompanionNode() = default;

MeshPtr CompanionNode::createMesh() {
  _store.reset(new DataStore(_flash, rtc()));
#ifdef WITH_RELIABLE_DM
  _rdm_io.reset(new rdm::ArduinoFileIO(_flash));
  _firmware = new DeterministicPeerData<MyMesh>(radio(), rng(), rtc(), tables(), *_store, *_rdm_io);
#else
  _firmware = new DeterministicPeerData<MyMesh>(radio(), rng(), rtc(), tables(), *_store);
#endif
  return ownMesh(_firmware);
}

// Same order as examples/companion_radio/main.cpp setup(): store.begin(), the_mesh.begin(), startInterface().
void CompanionNode::beginMesh() {
  _store->begin();
#ifdef WITH_RELIABLE_DM
  _firmware->setHardwareRTC(has_rtc_battery());
#endif
  _firmware->begin();
  _firmware->setBLEPin(0);
  _firmware->startInterface(_link);
  _app.onRadioBoot();
}

void CompanionNode::loopMesh() { _firmware->loop(); }

void CompanionNode::onLoop() { _app.tick(); }

// MyMesh owns its packet pool, so ask the firmware (held packets, outbound queue, lazy contacts write) and the app.
bool CompanionNode::busy() const {
  return (_firmware && (_firmware->holdsPackets() || _firmware->hasPendingWork())) || !_app.idle() ||
         !_link.to_app.empty() || !_link.to_radio.empty();
}

bool CompanionNode::rtcTrusted() {
  if (!powered() || !_firmware) return false;
#ifdef WITH_RELIABLE_DM
  return _firmware->rtcTrusted();
#else
  return has_rtc_battery() || rtc().setSinceBoot();
#endif
}

uint64_t CompanionNode::nextWakeupMs() const {
  uint64_t w = _app.nextDeadlineMs();
#ifdef WITH_RELIABLE_DM
  if (powered() && _firmware) {
    uint32_t f = _firmware->rdm().nextWakeupMillis((uint32_t)uptimeMs());
    if (f != UINT32_MAX) w = std::min(w, sim().now() + f);
  }
#endif
  return w;
}

void CompanionNode::onShutdown() {
  _app.onRadioBoot();
  _firmware = nullptr;
}

}
