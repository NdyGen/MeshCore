#include "Sim.h"

#include <Packet.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

namespace sim {

static Simulator* g_active = nullptr;

Simulator* activeSimulator() { return g_active; }

// ---------------------------------------------------------------- SimRNG

uint64_t SimRNG::next() {  // splitmix64
  uint64_t z = (_state += 0x9E3779B97F4A7C15ULL);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31);
}

void SimRNG::random(uint8_t* dest, size_t sz) {
  while (sz > 0) {
    uint64_t v = next();
    size_t n = sz < 8 ? sz : 8;
    memcpy(dest, &v, n);
    dest += n;
    sz -= n;
  }
}

static uint64_t mixSeed(uint64_t a, uint64_t b, uint64_t c) {
  SimRNG r;
  r.seed(a ^ (b * 0x100000001B3ULL) ^ (c << 32));
  return r.next();
}

// ---------------------------------------------------------------- clocks

unsigned long SimMillis::getMillis() { return (unsigned long)_node.uptimeMs(); }

SimRTC::SimRTC(const Simulator& sim, uint32_t epoch) : _sim(sim), _base_epoch(epoch), _base_ms(sim.now()) {}

uint32_t SimRTC::getCurrentTime() { return _base_epoch + (uint32_t)((_sim.now() - _base_ms) / 1000); }

void SimRTC::setCurrentTime(uint32_t time) {
  _base_epoch = time;
  _base_ms = _sim.now();
  _set_since_boot = true;
}

// ---------------------------------------------------------------- SimRadio

int SimRadio::recvRaw(uint8_t* bytes, int sz) {
  if (_rx_ready.empty()) return 0;
  Frame f = std::move(_rx_ready.front());
  _rx_ready.erase(_rx_ready.begin());
  int len = (int)std::min<size_t>(f.bytes.size(), (size_t)sz);
  memcpy(bytes, f.bytes.data(), len);
  _last_snr = f.snr;
  _last_rssi = -120.0f + f.snr;
  return len;
}

uint32_t SimRadio::getEstAirtimeFor(int len_bytes) { return _sim.airtime_ms(len_bytes); }

// Same model as RadioLibWrapper::packetScoreInt() (src/helpers/radiolib/RadioLibWrappers.cpp).
float SimRadio::packetScore(float snr, int packet_len) {
  static const float snr_threshold[] = {-7.5f, -10.0f, -12.5f, -15.0f, -17.5f, -20.0f};
  uint8_t sf = _sim.radio_cfg.sf;
  if (sf < 7 || sf > 12) return 0.0f;
  if (snr < snr_threshold[sf - 7]) return 0.0f;
  float success = (snr - snr_threshold[sf - 7]) / 10.0f;
  float collision_penalty = 1.0f - (packet_len / 256.0f);
  return std::max(0.0f, std::min(1.0f, success * collision_penalty));
}

bool SimRadio::startSendRaw(const uint8_t* bytes, int len) {
  _transmitting = true;
  _tx_end = _sim.now() + _sim.airtime_ms(len);
  _sim.transmit(_node, bytes, len);
  return true;
}

bool SimRadio::isSendComplete() { return _sim.now() >= _tx_end; }

bool SimRadio::isReceiving() { return _sim.receptionActiveAt(_node.index()); }

// ---------------------------------------------------------------- SimNode

SimNode::SimNode(Simulator& sim, std::string name, int index) : _sim(sim), _name(std::move(name)), _index(index) {
  _fs.on_write = [this](const std::string& path, uint32_t off, uint32_t len, bool complete) {
    _sim.log_fs_write(_index, path, off, len, complete);
  };
}

SimNode::~SimNode() = default;

uint64_t SimNode::uptimeMs() const { return _powered ? _sim.now() - _boot_ms : 0; }

void SimNode::loadOrCreateIdentity() {
  auto it = _fs.files.find("/identity");
  if (it != _fs.files.end()) {
    _identity.readFrom(it->second.data(), it->second.size());
    return;
  }
  _identity = mesh::LocalIdentity(&_rng);
  uint8_t buf[PRV_KEY_SIZE + PUB_KEY_SIZE];
  size_t n = _identity.writeTo(buf, sizeof(buf));
  _fs.files["/identity"] = std::vector<uint8_t>(buf, buf + n);
}

void SimNode::boot() {
  _boot_count++;
  _power_epoch++;
  _powered = true;
  _boot_ms = _sim._now;
  _rng.seed(mixSeed(_sim._seed, (uint64_t)_index + 1, _boot_count));

  uint32_t epoch;
  if (_boot_count == 1) {
    epoch = _sim.wall_epoch();
  } else if (_rtc_battery) {
    epoch = _rtc_epoch_at_off + (uint32_t)((_sim._now - _off_at_ms) / 1000);
  } else {
    epoch = RTC_RESET_EPOCH;
  }

  _ms.reset(new SimMillis(*this));
  _rtc.reset(new SimRTC(_sim, epoch));
  _radio.reset(new SimRadio(_sim, *this));
  _mgr.reset(new StaticPoolPacketManager(pool_size));
  _tables.reset(new SimpleMeshTables());
  _pending_reboot = _pending_power_off = false;
  if (_sim_identity) loadOrCreateIdentity();

  SimNode* prev = _sim._current;
  _sim._current = this;
  _mesh.reset(createMesh());
  if (_sim_identity) _mesh->self_id = _identity;
  beginMesh();
  _identity = _mesh->self_id;
  onBoot();
  _sim._current = prev;
}

void SimNode::shutdown() {
  if (!_powered) return;
  SimNode* prev = _sim._current;
  _sim._current = this;
  onShutdown();
  _sim._current = prev;

  _rtc_epoch_at_off = _rtc->getCurrentTime();
  _off_at_ms = _sim._now;
  // The packet pool is not freed by StaticPoolPacketManager; leaking it per boot is accepted in tests.
  _mesh.reset();
  _tables.reset();
  _mgr.reset();
  _radio.reset();
  _rtc.reset();
  _ms.reset();
  _powered = false;
  _power_epoch++;
}

void SimNode::power_off() { shutdown(); }

void SimNode::power_on() {
  if (!_powered) boot();
}

void SimNode::reboot(bool wipe_fs) {
  shutdown();
  if (wipe_fs) _fs.wipe();
  boot();
}

void SimNode::sync_clock() {
  if (_powered) _rtc->setCurrentTime(_sim.wall_epoch());
}

void SimNode::loop() {
  loopMesh();
  onLoop();
}

// ---------------------------------------------------------------- Simulator

enum DropReason { DROP_NONE = 0, DROP_COLLISION, DROP_HALFDUPLEX, DROP_OFF, DROP_LOSS, DROP_FILTER };

bool SimNode::busy() const { return _mgr && _mgr->getFreeCount() < pool_size; }

Simulator::Simulator(uint64_t seed, RadioConfig cfg) : _seed(seed), radio_cfg(cfg) {
  if (g_active) throw std::logic_error("only one Simulator may exist at a time");
  g_active = this;
  _rng.seed(mixSeed(seed, 0, 0));
  const char* t = getenv("SIM_TRACE");
  _trace = t && *t && *t != '0';
}

Simulator::~Simulator() {
  for (auto& n : _nodes) n->shutdown();
  _nodes.clear();
  g_active = nullptr;
}

void Simulator::link(SimNode& a, SimNode& b, float snr, float loss) {
  link_oneway(a, b, snr, loss);
  link_oneway(b, a, snr, loss);
}

void Simulator::link_oneway(SimNode& from, SimNode& to, float snr, float loss) {
  _links[{from.index(), to.index()}] = Link{snr, loss};
}

void Simulator::unlink(SimNode& a, SimNode& b) {
  _links.erase({a.index(), b.index()});
  _links.erase({b.index(), a.index()});
}

bool Simulator::linked(const SimNode& from, const SimNode& to) const {
  return _links.count({from.index(), to.index()}) != 0;
}

// LoRa time-on-air (Semtech AN1200.13), explicit header, CRC on.
uint32_t Simulator::airtime_ms(int len_bytes) const {
  const double tsym_ms = (double)(1u << radio_cfg.sf) / radio_cfg.bw_khz;
  const int de = tsym_ms > 16.0 ? 1 : 0;
  const double t_pre = (radio_cfg.preamble + 4.25) * tsym_ms;
  const double num = 8.0 * len_bytes - 4.0 * radio_cfg.sf + 28 + 16;
  const double den = 4.0 * (radio_cfg.sf - 2 * de);
  const double n_payload = 8 + std::max(std::ceil(num / den) * radio_cfg.cr, 0.0);
  return (uint32_t)std::ceil(t_pre + n_payload * tsym_ms);
}

bool Simulator::receptionActiveAt(int node_index) const {
  for (const auto& r : _in_flight) {
    if (r.to == node_index && r.start <= _now && _now < r.end) return true;
  }
  return false;
}

void Simulator::traceTx(const SimNode& from, const uint8_t* bytes, int len) {
  TxRecord rec;
  rec.seq = _event_seq++;
  rec.airtime_ms = airtime_ms(len);
  rec.t_ms = _now;
  rec.from = from.index();
  rec.header = bytes[0];
  rec.payload_type = (bytes[0] >> PH_TYPE_SHIFT) & PH_TYPE_MASK;
  uint8_t route = bytes[0] & PH_ROUTE_MASK;
  rec.flood = route == ROUTE_TYPE_FLOOD || route == ROUTE_TYPE_TRANSPORT_FLOOD;
  rec.raw.assign(bytes, bytes + len);
  _tx_log.push_back(rec);
  if (_trace) {
    printf("[%9.3fs] %-10s TX %-9s %s len=%d air=%ums\n", _now / 1000.0, from.name().c_str(),
           payloadTypeName(rec.payload_type), rec.flood ? "flood " : "direct", len, airtime_ms(len));
  }
}

void Simulator::transmit(SimNode& from, const uint8_t* bytes, int len) {
  traceTx(from, bytes, len);
  const TxRecord& tx = _tx_log.back();
  const uint64_t end = _now + airtime_ms(len);
  _last_activity = end;

  for (auto& r : _in_flight) {  // half duplex: the transmitter loses whatever it was receiving
    if (r.to == from.index() && r.corrupted == DROP_NONE) r.corrupted = DROP_HALFDUPLEX;
  }

  for (auto& np : _nodes) {
    SimNode& to = *np;
    if (to.index() == from.index()) continue;
    auto it = _links.find({from.index(), to.index()});
    if (it == _links.end()) continue;

    Reception r;
    r.to = to.index();
    r.from = from.index();
    r.bytes.assign(bytes, bytes + len);
    r.start = _now;
    r.end = end;
    r.snr = it->second.snr;
    r.loss = it->second.loss;
    r.corrupted = DROP_NONE;
    r.to_power_epoch = to.powerEpoch();

    if (!to.powered()) {
      r.corrupted = DROP_OFF;
    } else if (to.radio().isTransmitting()) {
      r.corrupted = DROP_HALFDUPLEX;
    }
    if (_collisions) {
      for (auto& other : _in_flight) {
        if (other.to == r.to && other.end > r.start) {
          if (other.corrupted == DROP_NONE) other.corrupted = DROP_COLLISION;
          if (r.corrupted == DROP_NONE) r.corrupted = DROP_COLLISION;
        }
      }
    }
    if (r.loss > 0.0f && _rng.uniform() < r.loss && r.corrupted == DROP_NONE) r.corrupted = DROP_LOSS;
    for (auto& f : _drop_filters) {
      if (f(tx, r.to)) {
        if (r.corrupted == DROP_NONE) r.corrupted = DROP_FILTER;
        break;
      }
    }
    _in_flight.push_back(std::move(r));
  }
}

void Simulator::deliverDue() {
  std::vector<Reception> due;
  for (auto it = _in_flight.begin(); it != _in_flight.end();) {
    if (it->end <= _now) {
      due.push_back(std::move(*it));
      it = _in_flight.erase(it);
    } else {
      ++it;
    }
  }
  std::stable_sort(due.begin(), due.end(), [](const Reception& a, const Reception& b) { return a.end < b.end; });

  for (auto& r : due) {
    SimNode& to = *_nodes[r.to];
    int reason = r.corrupted;
    if (reason == DROP_NONE && (!to.powered() || to.powerEpoch() != r.to_power_epoch)) reason = DROP_OFF;
    switch (reason) {
      case DROP_NONE:
        to.radio().deliver(r.bytes, r.snr);
        if (_trace) {
          printf("[%9.3fs] %-10s RX %-9s from %s\n", _now / 1000.0, to.name().c_str(),
                 payloadTypeName((r.bytes[0] >> PH_TYPE_SHIFT) & PH_TYPE_MASK), _nodes[r.from]->name().c_str());
        }
        break;
      case DROP_COLLISION: _dropped_collision++; break;
      case DROP_HALFDUPLEX: _dropped_halfduplex++; break;
      case DROP_OFF: _dropped_off++; break;
      case DROP_LOSS: _dropped_loss++; break;
      case DROP_FILTER: _dropped_filter++; break;
    }
  }
}

bool Simulator::idle() const {
  if (!_in_flight.empty() || _now < _last_activity + _ff_settle) return false;
  for (const auto& n : _nodes) {
    if (!n->powered()) continue;
    if (n->busy() || n->radio().isTransmitting() || n->radio().hasPendingRx()) return false;
  }
  return true;
}

uint64_t Simulator::nextStep() const {
  if (_ff_max_step == 0 || !idle()) return _tick;
  uint64_t step = _ff_max_step;
  for (const auto& n : _nodes) {
    if (!n->powered()) continue;
    uint64_t w = n->nextWakeupMs();
    if (w != UINT64_MAX) step = std::min(step, w > _now ? w - _now : (uint64_t)_tick);
  }
  return std::max<uint64_t>(step, _tick);
}

void Simulator::drop_next(uint8_t payload_type, int from_index, int to_index, int count) {
  auto remaining = std::make_shared<int>(count);
  auto last_seq = std::make_shared<int64_t>(-1);
  add_drop_filter([=](const TxRecord& tx, int to) {
    if (tx.payload_type != payload_type) return false;
    if (from_index >= 0 && tx.from != from_index) return false;
    if (to_index >= 0 && to != to_index) return false;
    if ((int64_t)tx.seq == *last_seq) return true;  // same transmission, other receivers
    if (*remaining <= 0) return false;
    (*remaining)--;
    *last_seq = (int64_t)tx.seq;
    return true;
  });
}

void Simulator::step(uint64_t limit) {
  _now += std::min(nextStep(), std::max<uint64_t>(limit, 1));
  deliverDue();
  for (auto& n : _nodes) {
    if (!n->powered()) continue;
    _current = n.get();
    n->loop();
    _current = nullptr;
    if (n->_fs.power_off_on_exhaust && n->_fs.write_budget == 0) {
      n->_fs.power_off_on_exhaust = false;
      n->_fs.write_budget = -1;
      n->power_off();
    } else if (n->_pending_power_off) {
      n->power_off();
    } else if (n->_pending_reboot) {
      n->reboot();
    }
  }
}

void Simulator::run_for(uint64_t ms) {
  const uint64_t until = _now + ms;
  while (_now < until) step(until - _now);
}

bool Simulator::run_until(const std::function<bool()>& pred, uint64_t timeout_ms) {
  const uint64_t until = _now + timeout_ms;
  while (!pred()) {
    if (_now >= until) return false;
    step(until - _now);
  }
  return true;
}

void Simulator::log_fs_write(int node, const std::string& path, uint32_t off, uint32_t len, bool complete) {
  _fs_log.push_back(FsWrite{_event_seq++, _now, node, path, off, len, complete});
}

size_t Simulator::count_tx(int from_index, int payload_type) const {
  size_t n = 0;
  for (const auto& t : _tx_log) {
    if (from_index >= 0 && t.from != from_index) continue;
    if (payload_type >= 0 && t.payload_type != payload_type) continue;
    n++;
  }
  return n;
}

const char* payloadTypeName(uint8_t type) {
  switch (type) {
    case PAYLOAD_TYPE_REQ: return "REQ";
    case PAYLOAD_TYPE_RESPONSE: return "RESPONSE";
    case PAYLOAD_TYPE_TXT_MSG: return "TXT_MSG";
    case PAYLOAD_TYPE_ACK: return "ACK";
    case PAYLOAD_TYPE_ADVERT: return "ADVERT";
    case PAYLOAD_TYPE_GRP_TXT: return "GRP_TXT";
    case PAYLOAD_TYPE_GRP_DATA: return "GRP_DATA";
    case PAYLOAD_TYPE_ANON_REQ: return "ANON_REQ";
    case PAYLOAD_TYPE_PATH: return "PATH";
    case PAYLOAD_TYPE_TRACE: return "TRACE";
    case PAYLOAD_TYPE_MULTIPART: return "MULTIPART";
    case PAYLOAD_TYPE_CONTROL: return "CONTROL";
    case PAYLOAD_TYPE_RAW_CUSTOM: return "RAW";
    default: return "?";
  }
}

}
