#include "RdmSimCompanion.h"

#include <algorithm>
#include <string.h>

namespace sim {

static const char* CONTACTS_FILE = "/contacts";

class RdmSimCompanion::Impl : public DeterministicPeerData<RdmChatMesh>, private rdm::NodeAppSink {
  RdmSimCompanion& _n;

public:
  Impl(RdmSimCompanion& n, mesh::Radio& radio, mesh::MillisecondClock& ms, mesh::RNG& rng, mesh::RTCClock& rtc,
       mesh::PacketManager& mgr, mesh::MeshTables& tables, rdm::FileIO& io)
      : DeterministicPeerData<RdmChatMesh>(radio, ms, rng, rtc, mgr, tables, io, *this), _n(n) {}

  rdm::Node& node() { return rdm(); }
  bool nextInbox(rdm::InRecord& r) { return rdmNextInbox(r); }

  // CMD_SEND_TXT_MSG under WITH_RELIABLE_DM: handled by the outbox, or the upstream way.
  uint32_t sendApp(ContactInfo& c, uint32_t ts, uint8_t attempt, const std::string& text, bool& handled) {
    AppSend a = rdmSendApp(c, ts, attempt, text.data(), text.size());
    handled = a.handled;
    if (a.handled) return a.app_ack;
    uint32_t expected = 0, est = 0;
    if (sendMessage(c, ts, attempt, text.c_str(), expected, est) != MSG_SEND_FAILED && expected) {
      UpstreamAck u;
      u.ack = expected;
      memcpy(u.pub, c.id.pub_key, PUB_KEY_SIZE);
      _n._upstream_acks.push_back(u);
    }
    return expected;
  }

private:
  // rdm::NodeAppSink
  uint32_t rtcNow(bool& trusted) override {
    trusted = _n.has_rtc_battery() || _n.rtc().setSinceBoot();
    return getRTCClock()->getCurrentTime();
  }
  bool pushUserStatus(const uint8_t pub_prefix[6], uint32_t app_ack, rdm::UserStatus s, const uint8_t key[4],
                      uint32_t ts) override {
    (void)key;
    if (!_n._connected || !_n._rdm_client) return false;
    StatusPush p{_n.sim().now(), app_ack, s, ts, {0}};
    memcpy(p.prefix, pub_prefix, 6);
    _n._statuses.push_back(p);
    return true;
  }
  bool pushSendConfirmed(uint32_t app_ack) override {
    if (!_n._connected) return false;
    _n._confirms.push_back(app_ack);
    return true;
  }
  void pushMsgWaiting() override { _n._sync_wanted = true; }

protected:
  // BaseChatMesh
  void onDiscoveredContact(ContactInfo&, bool, uint8_t, const uint8_t*) override { _n.saveContacts(); }
  void onContactPathUpdated(const ContactInfo&) override { _n.saveContacts(); }
  void rdmOnContactRemoved(const ContactInfo& removed) override {
    _n._removed.emplace_back(removed.id.pub_key, removed.id.pub_key + PUB_KEY_SIZE);
    _n.saveContacts();
  }
  ContactInfo* processAck(const uint8_t* data) override {   // upstream sends only (RDM off, TOO_BIG, NO_OUTBOX)
    uint32_t ack;
    memcpy(&ack, data, 4);
    auto& v = _n._upstream_acks;
    for (auto it = v.begin(); it != v.end(); ++it) {
      if (it->ack != ack) continue;
      ContactInfo* c = lookupContactByPubKey(it->pub, PUB_KEY_SIZE);
      v.erase(it);
      if (_n._connected) _n._confirms.push_back(ack);
      return c;
    }
    return NULL;
  }
  void onMessageRecv(const ContactInfo& from, mesh::Packet*, uint32_t ts, const char* text) override {
    _n._upstream_queue.push_back({_n.indexForPubKey(from.id.pub_key), text, ts, _n.sim().now(), false});
    _n._sync_wanted = true;
  }
  void onCommandDataRecv(const ContactInfo&, mesh::Packet*, uint32_t, const char*) override {}
  void onCLICommandRecv(const ContactInfo&, mesh::Packet*, uint32_t, const char*, char*) override {}
  void onSignedMessageRecv(const ContactInfo&, mesh::Packet*, uint32_t, const uint8_t*, const char*) override {}
  uint32_t calcFloodTimeoutMillisFor(uint32_t airtime) const override {
    return SEND_TIMEOUT_BASE_MILLIS + (FLOOD_SEND_TIMEOUT_FACTOR * airtime);
  }
  uint32_t calcDirectTimeoutMillisFor(uint32_t airtime, uint8_t path_len) const override {
    return SEND_TIMEOUT_BASE_MILLIS +
           ((airtime * DIRECT_SEND_PERHOP_FACTOR + DIRECT_SEND_PERHOP_EXTRA_MILLIS) * ((path_len & 63) + 1));
  }
  void onSendTimeout() override {}
  void onChannelMessageRecv(const mesh::GroupChannel&, mesh::Packet*, uint32_t, const char*) override {}
  uint8_t onContactRequest(const ContactInfo&, uint32_t, const uint8_t*, uint8_t, uint8_t*) override { return 0; }
  void onContactResponse(const ContactInfo&, const uint8_t*, uint8_t) override {}
};

// ---------------------------------------------------------------- LoggingIO

bool RdmSimCompanion::LoggingIO::create(const char* p, uint32_t n) {
  bool ok = _io.create(p, n);
  _fs.report_write(p, 0, n, ok);
  return ok;
}

bool RdmSimCompanion::LoggingIO::write(const char* p, uint32_t off, const uint8_t* b, uint32_t n) {
  bool ok = _io.write(p, off, b, n);
  _fs.report_write(p, off, n, ok);
  return ok;
}

// ---------------------------------------------------------------- RdmSimCompanion

RdmSimCompanion::RdmSimCompanion(Simulator& sim, std::string name, int index)
    : SimNode(sim, std::move(name), index), _io(fs()) {}

RdmSimCompanion::~RdmSimCompanion() = default;

mesh::Mesh* RdmSimCompanion::createMesh() {
  _impl = new Impl(*this, radio(), millisClock(), rng(), rtc(), packetManager(), tables(), _io);
  return _impl;
}

// Same order as the companion: mesh, contacts from flash, then RDM on the mounted flash. The link drops on reboot.
void RdmSimCompanion::beginMesh() {
  _impl->begin();
  loadContacts();
  _impl->node().begin();
  _connected = _rdm_client = _sync_wanted = false;
}

void RdmSimCompanion::loopMesh() {
  _impl->loop();
  _impl->node().loop();
}

void RdmSimCompanion::onLoop() {
  if (_sync_wanted && _connected && auto_sync) sync();
}

bool RdmSimCompanion::busy() const { return SimNode::busy() || (_sync_wanted && _connected && auto_sync); }

uint64_t RdmSimCompanion::nextWakeupMs() const {
  if (!powered() || !_impl) return UINT64_MAX;
  uint32_t w = _impl->node().nextWakeupMillis((uint32_t)uptimeMs());
  return w == UINT32_MAX ? UINT64_MAX : const_cast<RdmSimCompanion*>(this)->sim().now() + w;
}

rdm::Node& RdmSimCompanion::rdm() { return _impl->node(); }

void RdmSimCompanion::connect(bool rdm_client) {
  if (!powered()) return;
  Simulator::OnNode ctx(sim(), *this);
  _connected = true;
  _rdm_client = rdm_client;
  sync_clock();
  rdm().onClientConnected(rdm_client);
  sync();
}

void RdmSimCompanion::disconnect() {
  if (!powered()) return;
  Simulator::OnNode ctx(sim(), *this);
  _connected = false;
  rdm().onClientDisconnected();
}

int RdmSimCompanion::send_text(const SimNode& to, const std::string& text, uint8_t attempt, uint32_t ts) {
  if (!powered() || !_connected) return -1;
  ContactInfo* c = _impl->lookupContactByPubKey(to.identity().pub_key, PUB_KEY_SIZE);
  if (!c) return -1;
  Simulator::OnNode ctx(sim(), *this);
  if (!ts) ts = sim().wall_epoch();
  bool handled = false;
  uint32_t ack = _impl->sendApp(*c, ts, attempt, text, handled);
  _sent.push_back({ack, ts, text, to.index(), handled, sim().now()});
  return (int)_sent.size() - 1;
}

// CMD_SYNC_NEXT_MESSAGE until NO_MORE_MESSAGES: each request confirms the frame handed before it.
void RdmSimCompanion::sync() {
  _sync_wanted = false;
  if (!powered() || !_connected) return;
  Simulator::OnNode ctx(sim(), *this);
  for (;;) {
    rdm::InRecord r;
    if (!_impl->nextInbox(r)) break;
    _inbox.push_back({indexForPubKey(r.sender_prefix), std::string(r.text, r.text_len), r.ts, sim().now(), true});
  }
  for (auto& m : _upstream_queue) _inbox.push_back(m);
  _upstream_queue.clear();
}

size_t RdmSimCompanion::inbox_count(const std::string& text) const {
  size_t n = 0;
  for (const auto& m : _inbox) n += m.text == text;
  return n;
}

std::vector<rdm::UserStatus> RdmSimCompanion::statuses_for(uint32_t app_ack) const {
  std::vector<rdm::UserStatus> out;
  for (const auto& s : _statuses)
    if (s.app_ack == app_ack) out.push_back(s.s);
  return out;
}

void RdmSimCompanion::advert(bool flood) {
  if (!powered()) return;
  Simulator::OnNode ctx(sim(), *this);
  mesh::Packet* pkt = _impl->createSelfAdvert(name().c_str());
  if (!pkt) return;
  if (flood) _impl->sendFlood(pkt); else _impl->sendZeroHop(pkt);
}

void RdmSimCompanion::add_contact(const SimNode& other, bool favourite) {
  if (!powered()) return;
  ContactInfo* existing = _impl->lookupContactByPubKey(other.identity().pub_key, PUB_KEY_SIZE);
  if (existing) {
    existing->flags = favourite ? (existing->flags | 0x01) : (existing->flags & ~0x01);
  } else {
    ContactInfo c;
    memset(&c, 0, sizeof(c));
    c.id = other.identity();
    strncpy(c.name, other.name().c_str(), sizeof(c.name) - 1);
    c.type = ADV_TYPE_CHAT;
    c.flags = favourite ? 0x01 : 0;
    c.out_path_len = OUT_PATH_UNKNOWN;
    c.lastmod = rtc().getCurrentTime();
    _impl->addContact(c);
  }
  saveContacts();
}

bool RdmSimCompanion::knows(const SimNode& other) {
  return powered() && _impl->lookupContactByPubKey(other.identity().pub_key, PUB_KEY_SIZE) != NULL;
}

bool RdmSimCompanion::has_path_to(const SimNode& other) {
  ContactInfo* c = powered() ? _impl->lookupContactByPubKey(other.identity().pub_key, PUB_KEY_SIZE) : NULL;
  return c && c->out_path_len != OUT_PATH_UNKNOWN;
}

bool RdmSimCompanion::set_mailbox(const uint8_t* mbx_pub, const uint8_t* k_owner) {
  if (!powered()) return false;
  Simulator::OnNode ctx(sim(), *this);
  return rdm().setOwnMailbox(mbx_pub, k_owner);
}

int RdmSimCompanion::indexForPubKey(const uint8_t* pub) const {
  Simulator& s = const_cast<RdmSimCompanion*>(this)->sim();
  for (size_t i = 0; i < s.node_count(); i++)
    if (memcmp(s.node(i).identity().pub_key, pub, 6) == 0) return (int)i;
  return -1;
}

void RdmSimCompanion::saveContacts() {
  std::vector<uint8_t> blob;
  ContactInfo c;
  for (int i = 0; i < _impl->getTotalContactSlots(); i++) {
    if (!_impl->getContactByIdx(i, c) || c.type == ADV_TYPE_NONE) continue;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&c);
    blob.insert(blob.end(), p, p + sizeof(c));
  }
  fs().files[CONTACTS_FILE] = blob;
}

void RdmSimCompanion::loadContacts() {
  auto it = fs().files.find(CONTACTS_FILE);
  if (it == fs().files.end()) return;
  const std::vector<uint8_t>& blob = it->second;
  for (size_t off = 0; off + sizeof(ContactInfo) <= blob.size(); off += sizeof(ContactInfo)) {
    ContactInfo c;
    memcpy(reinterpret_cast<uint8_t*>(&c), &blob[off], sizeof(c));
    _impl->addContact(c);
  }
}

}
