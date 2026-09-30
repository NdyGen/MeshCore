#pragma once

// Nodes on a fake wire (no Mesh, no radio): what RdmChatMesh does with packets is done here by hand, so
// test_rdm_node checks rdm::Node and its modules without the packet layer.

#include <helpers/rdm/RdmCodec.h>
#include <helpers/rdm/RdmCrypto.h>
#include <helpers/rdm/RdmNode.h>

#include <MemFileIO.h>
#include <TestUtil.h>

#include <deque>
#include <functional>
#include <memory>
#include <string.h>
#include <string>
#include <vector>

namespace wire {

using namespace rdm;

enum class Kind { TXT, ACK, REQ, RESP, ANON };

struct Msg {
  Kind kind;
  int from, to;
  std::vector<uint8_t> bytes;
  uint32_t t_ms;
};

class Net;

struct StatusPush { uint32_t app_ack; UserStatus s; uint32_t ts; };

class Peer : public NodeMeshHost, public NodeAppSink {
public:
  Net& net;
  int id;
  uint8_t pub[32];
  MemFileIO io;
  std::unique_ptr<Node> node;
  bool online = true;
  struct Contact { int peer; bool fav; bool path; };
  std::vector<Contact> contacts;
  std::vector<StatusPush> statuses;
  std::vector<uint32_t> confirms;
  int msg_waiting = 0;
  Lcg32 rnd{1, 1103515245u, 12345u};
  // mailbox role (answers FETCH/DEPOSIT itself instead of running a Node)
  std::function<void(Peer& self, const Msg& m)> on_req;

  Peer(Net& n, int i, uint32_t capacity);
  void boot() { node.reset(new Node(io, *this, *this)); node->begin(); }
  void reboot() { boot(); }
  const uint8_t* prefix() const { return pub; }
  Contact* contactOf(const uint8_t* prefix);

  uint32_t millis() override;
  uint32_t rtcNow(bool& trusted) override;
  uint32_t random32() override { return rnd.next(); }
  bool txIdle() override { return true; }
  void selfPub(uint8_t out[32]) override { memcpy(out, pub, 32); }
  bool lookupContact(const uint8_t prefix[6], uint8_t pub_out[32], bool& fav, bool& has_path) override;
  bool sendTxtPlain(const uint8_t prefix[6], const uint8_t* plain, size_t len, bool flood, uint32_t& est) override;
  bool sendAck(const uint8_t prefix[6], const uint8_t* ack, uint8_t len) override;
  bool sendReq(const uint8_t peer_pub[32], uint32_t ts, const uint8_t* body, size_t len, bool flood, uint32_t& est) override;
  bool sendAnonReq(const uint8_t peer_pub[32], const uint8_t* plain, size_t len, uint32_t& est) override;
  bool encryptTxtPayload(const uint8_t prefix[6], const uint8_t* plain, size_t len, uint8_t* out, size_t& out_len) override;
  bool decryptTxtPayload(const uint8_t* payload, size_t len, uint8_t sender_out[32], uint8_t* plain, size_t& plain_len) override;
  bool pushUserStatus(const uint8_t prefix[6], uint32_t app_ack, UserStatus s, const uint8_t key[4], uint32_t ts) override;
  bool pushSendConfirmed(uint32_t app_ack) override { confirms.push_back(app_ack); return true; }
  void pushMsgWaiting() override { msg_waiting++; }

  // what a companion does for CMD_SEND_TXT_MSG: Node::appSend, or the upstream DM (no trailer) when not handled
  Node::AppSend appSend(Peer& to, const std::string& text, uint8_t attempt = 0, uint32_t ts = 0);
  // CMD_SYNC_NEXT_MESSAGE until empty; returns the texts handed to the app
  std::vector<std::string> appSync();
  bool hasStatus(UserStatus s) const;
};

class Net {
public:
  uint32_t ms = 1000;
  uint32_t epoch = 1790640000;
  std::vector<std::unique_ptr<Peer>> peers;
  std::deque<Msg> queue;
  std::vector<Msg> log;   // every message put on the wire
  std::function<bool(const Msg&)> drop;   // true: lose it

  Peer& add(uint32_t capacity = 1024 * 1024) {
    peers.emplace_back(new Peer(*this, (int)peers.size(), capacity));
    return *peers.back();
  }
  void befriend(Peer& a, Peer& b, bool fav = true) {
    a.contacts.push_back({b.id, fav, true});
    b.contacts.push_back({a.id, fav, true});
  }
  Peer* byPrefix(const uint8_t* prefix, size_t n = 6);
  void send(Kind k, int from, int to, const uint8_t* data, size_t len);
  void pump();
  // advances the clock in `step_ms` steps, looping every online node and delivering after each step
  void run(uint32_t total_ms, uint32_t step_ms = 1000);
  size_t count(Kind k, int from = -1, int to = -1, int first_byte = -1) const;
};

}
