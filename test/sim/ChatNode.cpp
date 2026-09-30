#include "ChatNode.h"

#include <cstring>

namespace sim {

class ChatNode::Impl : public SimChatMeshBase {
  ChatNode& _node;

public:
  Impl(ChatNode& node, mesh::Radio& radio, mesh::MillisecondClock& ms, mesh::RNG& rng, mesh::RTCClock& rtc,
       mesh::PacketManager& mgr, mesh::MeshTables& tables)
      : SimChatMeshBase(radio, ms, rng, rtc, mgr, tables), _node(node) {}

  using BaseChatMesh::sendFloodScoped;

protected:
  void onDiscoveredContact(ContactInfo&, bool, uint8_t, const uint8_t*) override { _node.saveContacts(); }

  ContactInfo* processAck(const uint8_t* data) override {
    uint32_t ack;
    memcpy(&ack, data, 4);
    for (auto& m : _node._sent) {
      if (m.expected_ack == ack && m.acked_at_ms == 0) {
        _node.markAcked(ack);
        return _node.contactFor(_node.sim().node(m.to_index));
      }
    }
    return nullptr;
  }

  void onContactPathUpdated(const ContactInfo&) override { _node.saveContacts(); }

  void onMessageRecv(const ContactInfo& from, mesh::Packet* pkt, uint32_t ts, const char* text) override {
    record(from, pkt, ts, TXT_TYPE_PLAIN, text);
  }
  void onCommandDataRecv(const ContactInfo& from, mesh::Packet* pkt, uint32_t ts, const char* text) override {
    record(from, pkt, ts, TXT_TYPE_CLI_DATA, text);
  }
  void onCLICommandRecv(const ContactInfo& from, mesh::Packet* pkt, uint32_t ts, const char* text, char*) override {
    record(from, pkt, ts, TXT_TYPE_CLI_COMMAND, text);
  }
  void onSignedMessageRecv(const ContactInfo& from, mesh::Packet* pkt, uint32_t ts, const uint8_t*, const char* text) override {
    record(from, pkt, ts, TXT_TYPE_SIGNED_PLAIN, text);
  }

private:
  void record(const ContactInfo& from, mesh::Packet* pkt, uint32_t ts, uint8_t type, const char* text) {
    RxMsg m;
    m.from_index = _node.sim().indexOfPrefix(from.id.pub_key, PUB_KEY_SIZE);
    m.text = text;
    m.sender_timestamp = ts;
    m.txt_type = type;
    m.flood = pkt->isRouteFlood();
    m.path_len = pkt->isRouteFlood() ? pkt->path_len : 0xFF;
    m.at_ms = _node.sim().now();
    _node._inbox.push_back(m);
  }
};

ChatNode::ChatNode(Simulator& sim, std::string name, int index) : SimNode(sim, std::move(name), index) {}

ChatNode::~ChatNode() = default;

MeshPtr ChatNode::createMesh() {
  _impl = new Impl(*this, radio(), millisClock(), rng(), rtc(), packetManager(), tables());
  return ownMesh(_impl);
}

void ChatNode::onBoot() { SimContacts::load(chat(), fs()); }

void ChatNode::loopMesh() { _impl->loop(); }

BaseChatMesh& ChatNode::chat() { return *_impl; }

void ChatNode::onLoop() {
  const uint64_t now = sim().now();
  for (auto& m : _sent) {
    if (m.status == Status::PENDING && now >= m.sent_at_ms + m.est_timeout_ms) {
      m.status = Status::TIMED_OUT;
      m.timed_out_before_ack = true;
    }
  }
  _retries.tick([this](int id) { return _sent.at(id).status; },
                [this](int to) { reset_path(sim().node(to)); },
                [this](int to, const std::string& text, uint8_t attempt, uint32_t ts) {
                  return send_text(sim().node(to), text, attempt, ts);
                });
}

uint64_t ChatNode::nextWakeupMs() const {
  uint64_t w = UINT64_MAX;
  for (const auto& m : _sent) {
    if (m.status == Status::PENDING) w = std::min(w, m.sent_at_ms + m.est_timeout_ms);
  }
  return w;
}

void ChatNode::advert(bool flood) {
  if (!powered()) return;
  Simulator::OnNode ctx(sim(), *this);
  mesh::Packet* pkt = chat().createSelfAdvert(name().c_str());
  if (!pkt) return;
  if (flood) {
    _impl->sendFlood(pkt);
  } else {
    _impl->sendZeroHop(pkt);
  }
}

void ChatNode::add_contact(const SimNode& other) {
  if (!powered() || knows(other)) return;
  ContactInfo c;
  memset(&c, 0, sizeof(c));
  c.id = other.identity();
  strncpy(c.name, other.name().c_str(), sizeof(c.name) - 1);
  c.type = ADV_TYPE_CHAT;
  c.out_path_len = OUT_PATH_UNKNOWN;
  c.lastmod = rtc().getCurrentTime();
  chat().addContact(c);
  saveContacts();
}

ContactInfo* ChatNode::contactFor(const SimNode& other) {
  if (!powered()) return nullptr;
  return chat().lookupContactByPubKey(other.identity().pub_key, PUB_KEY_SIZE);
}

bool ChatNode::knows(const SimNode& other) { return contactFor(other) != nullptr; }

bool ChatNode::has_path_to(const SimNode& other) {
  ContactInfo* c = contactFor(other);
  return c && c->out_path_len != OUT_PATH_UNKNOWN;
}

uint8_t ChatNode::path_len_to(const SimNode& other) {
  ContactInfo* c = contactFor(other);
  return c ? c->out_path_len : OUT_PATH_UNKNOWN;
}

void ChatNode::reset_path(const SimNode& other) {
  ContactInfo* c = contactFor(other);
  if (!c) return;
  chat().resetPathTo(*c);
  saveContacts();
}

int ChatNode::send_text(const SimNode& to, const std::string& text, uint8_t attempt, uint32_t timestamp) {
  ContactInfo* c = contactFor(to);
  if (!c) return -1;
  Simulator::OnNode ctx(sim(), *this);

  SentMsg m{};
  m.id = (int)_sent.size();
  m.to_index = to.index();
  m.text = text;
  m.timestamp = timestamp ? timestamp : rtc().getCurrentTimeUnique();
  m.attempt = attempt;
  m.sent_at_ms = sim().now();
  int rc = chat().sendMessage(*c, m.timestamp, attempt, text.c_str(), m.expected_ack, m.est_timeout_ms);
  m.flood = rc == MSG_SEND_SENT_FLOOD;
  m.status = rc == MSG_SEND_FAILED ? Status::SEND_FAILED : Status::PENDING;
  _sent.push_back(m);
  return m.id;
}

int ChatNode::send_text_with_retries(const SimNode& to, const std::string& text, AppRetry policy) {
  int first = send_text(to, text, 0);
  if (first < 0) return -1;
  return _retries.add(to.index(), text, _sent[first].timestamp, policy, first);
}

ChatNode::Status ChatNode::retry_status(int retry_id) const {
  return _retries.status(retry_id, [this](int id) { return _sent.at(id).status; });
}

void ChatNode::markAcked(uint32_t ack) {
  for (auto& m : _sent) {
    if (m.expected_ack == ack && m.acked_at_ms == 0) {
      m.status = Status::DELIVERED;
      m.acked_at_ms = sim().now();
    }
  }
}

void ChatNode::saveContacts() { SimContacts::save(chat(), fs()); }

// ---------------------------------------------------------------- RepeaterNode

namespace {
class RepeaterMesh : public mesh::Mesh {
public:
  RepeaterMesh(mesh::Radio& radio, mesh::MillisecondClock& ms, mesh::RNG& rng, mesh::RTCClock& rtc,
               mesh::PacketManager& mgr, mesh::MeshTables& tables)
      : mesh::Mesh(radio, ms, rng, rtc, mgr, tables) {}

protected:
  bool allowPacketForward(const mesh::Packet*) override { return true; }
};
}

MeshPtr RepeaterNode::createMesh() {
  return ownMesh(new RepeaterMesh(radio(), millisClock(), rng(), rtc(), packetManager(), tables()));
}

void RepeaterNode::advert() {
  if (!powered()) return;
  Simulator::OnNode ctx(sim(), *this);
  uint8_t app_data[MAX_ADVERT_DATA_SIZE];
  AdvertDataBuilder builder(ADV_TYPE_REPEATER, name().c_str());
  uint8_t len = builder.encodeTo(app_data);
  mesh::Packet* pkt = mesh().createAdvert(identity(), app_data, len);
  if (pkt) mesh().sendFlood(pkt);
}

}
