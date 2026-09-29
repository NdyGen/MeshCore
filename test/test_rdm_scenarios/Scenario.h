#pragma once

// Scenario world for 06 par. 6.2: Bob and Alice as companion (RdmSimCompanion or the real MyMesh in CompanionNode)
// with an RdmSimApp each, repeater R between them,
// mailbox M (MemMailboxBackend, or the real mbxd through SubprocessBackend) at R. Everything a scenario needs
// beyond the simulator: setup up to the scenario's start state, running with the apps ticking, and decoded wires.

#include <gtest/gtest.h>

#include <ChatNode.h>
#include <CompanionNode.h>
#include <RdmScenario.h>
#include <RdmSimApp.h>
#include <RdmSimCompanion.h>
#include <RdmSimMailbox.h>
#include <SubprocessBackend.h>
#include <helpers/rdm/RdmContacts.h>

#include <stdlib.h>
#include <string.h>

#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace sim {

// Radio side of a companion for scenario setup and white-box checks, over either companion model.
class CompanionCtl {
public:
  virtual ~CompanionCtl() {}
  virtual SimNode& node() = 0;
  virtual RdmAppRadio& port() = 0;
  virtual void attach(RdmAppSink& sink) = 0;
  virtual void advert() = 0;
  virtual void addContact(const SimNode& other, bool favourite) = 0;
  virtual bool hasPathTo(const SimNode& other) = 0;
  virtual bool isContact(const SimNode& other) = 0;   // in contacts[], so the app shows it
  virtual rdm::Node& rdm() = 0;   // call under Simulator::OnNode for node()
};

// RdmAppRadio on RdmSimCompanion (WP5), whose app side is direct calls: replies come at once, and the pushes it
// records are handed to the app in appPoll().
class CompanionPort : public RdmAppRadio {
public:
  explicit CompanionPort(RdmSimCompanion& c) : _c(c) {}
  void attach(RdmAppSink& sink) { _sink = &sink; }

  void appPoll() override {
    if (!_sink) return;
    const auto& st = _c.statuses();
    static const uint8_t no_key[4] = { 0, 0, 0, 0 };
    for (; _status_seen < st.size(); _status_seen++) {
      _sink->pushRdmStatus(st[_status_seen].prefix, st[_status_seen].app_ack, st[_status_seen].s, no_key,
                           st[_status_seen].ts);
    }
    const auto& cf = _c.confirms();
    for (; _confirm_seen < cf.size(); _confirm_seen++) _sink->pushSendConfirmed(cf[_confirm_seen]);
    if (_c.inbox().size() > _inbox_seen) _sink->pushMsgWaiting();
  }
  bool appLinkUp() override { return _c.connected(); }
  void appConnect(uint32_t, bool rdm_enable, Done done) override {
    _c.auto_sync = true;   // the model syncs itself on MSG_WAITING; appSyncNext() hands those messages over
    _c.connect(rdm_enable);
    done(rdm_enable && _c.connected());
  }
  void appDisconnect() override { _c.disconnect(); }
  void appSendText(const uint8_t pub_prefix[6], uint32_t ts, uint8_t attempt, const std::string& text,
                   SendDone done) override {
    SendReply r = { false, false, 0, 0 };
    SimNode* to = nodeByPrefix(pub_prefix);
    int id = to ? _c.send_text(*to, text, attempt, ts) : -1;
    if (id >= 0) {
      r.ok = true;
      r.flood = !_c.has_path_to(*to);
      r.app_ack = _c.sent(id).app_ack;
      r.est_timeout_ms = estTimeout(text.size(), r.flood);
    }
    done(r);
  }
  void appResetPath(const uint8_t*, Done done) override { done(false); }
  void appSyncNext(SyncDone done) override {
    if (_queue.empty()) {
      _c.sync();
      const auto& in = _c.inbox();
      for (; _inbox_seen < in.size(); _inbox_seen++) {
        Msg m;
        memset(m.sender_prefix, 0, 6);
        if (in[_inbox_seen].from_index >= 0) {
          memcpy(m.sender_prefix, _c.sim().node((size_t)in[_inbox_seen].from_index).identity().pub_key, 6);
        }
        m.ts = in[_inbox_seen].sender_timestamp;
        m.txt_type = 0;
        m.text = in[_inbox_seen].text;
        _queue.push_back(m);
      }
    }
    Msg m;
    if (_queue.empty()) return done(false, m);
    m = _queue.front();
    _queue.pop_front();
    done(true, m);
  }
  void appSetMailbox(const uint8_t* mbx_pub, const uint8_t* k_owner, Done done) override {
    done(_c.set_mailbox(mbx_pub, k_owner));
  }
  void appListOutbox(ListDone done) override {
    std::vector<rdm::companion::OutboxRow> rows;
    if (_c.powered()) {
      Simulator::OnNode ctx(_c.sim(), _c);
      rdm::OutEntry e[RDM_OUTBOX_SLOTS_MAX];
      uint8_t n = _c.rdm().listOutbox(e, RDM_OUTBOX_SLOTS_MAX);
      for (uint8_t i = 0; i < n; i++) rows.push_back(rdm::companion::outboxRow(e[i], i));
    }
    done(rows);
  }

private:
  RdmSimCompanion& _c;
  RdmAppSink* _sink = nullptr;
  size_t _status_seen = 0, _confirm_seen = 0, _inbox_seen = 0;
  std::deque<Msg> _queue;

  SimNode* nodeByPrefix(const uint8_t* prefix) {
    for (size_t i = 0; i < _c.sim().node_count(); i++) {
      if (memcmp(_c.sim().node(i).identity().pub_key, prefix, 6) == 0) return &_c.sim().node(i);
    }
    return nullptr;
  }
  // RESP_CODE_SENT.est_timeout as MyMesh computes it (examples/companion_radio/MyMesh.cpp), for a fork DM with
  // trailer over one repeater: RdmSimCompanion does not report the one of its own send.
  uint32_t estTimeout(size_t text_len, bool flood) {
    size_t cipher = ((5 + text_len + 3 + 15) / 16) * 16;
    uint32_t air = _c.sim().airtime_ms((int)(2 + 1 + 4 + cipher));
    return flood ? 500 + 16 * air : 500 + (air * 6 + 250) * 2;
  }
};

// RdmAppRadio over companion frames to the real MyMesh (WP6) in CompanionNode, through CompanionApp's command queue
// (one command in flight, multi-frame replies up to their closing frame). Pushes come from its push log.
class FramePort : public RdmAppRadio {
public:
  explicit FramePort(CompanionNode& c) : _c(c) {}
  void attach(RdmAppSink& sink) { _sink = &sink; }

  void appPoll() override {
    const auto& log = _c.app().push_log();
    for (; _push_seen < log.size(); _push_seen++) {
      const std::vector<uint8_t>& f = log[_push_seen];
      if (!_sink || f.empty()) continue;
      rdm::companion::StatusPush p;
      if (rdm::companion::decodeStatus(f.data(), f.size(), p)) {
        _sink->pushRdmStatus(p.pub_prefix, p.app_ack, p.status, p.key, p.ts);
      } else if (f[0] == rdm::companion::PUSH_CODE_SEND_CONFIRMED && f.size() >= 5) {
        _sink->pushSendConfirmed(rdm::companion::get32(&f[1]));
      } else if (f[0] == PUSH_CODE_MSG_WAITING) {
        _sink->pushMsgWaiting();
      }
    }
  }
  bool appLinkUp() override { return _c.app().connected(); }
  void appConnect(uint32_t, bool rdm_enable, Done done) override {
    _c.app().auto_sync = false;   // RdmSimApp syncs
    _c.app().connect();           // CMD_DEVICE_QUERY, CMD_APP_START, CMD_SET_DEVICE_TIME with the phone clock
    if (!rdm_enable) return done(false);
    uint8_t f[2];
    rdm::companion::buildEnable(f);
    _c.app().send_command({ f, f + 2 }, [done](const std::vector<uint8_t>& r) { done(r[0] == RESP_CODE_OK); });
  }
  void appDisconnect() override { _c.app().disconnect(); }
  void appSendText(const uint8_t pub_prefix[6], uint32_t ts, uint8_t attempt, const std::string& text,
                   SendDone done) override {
    std::vector<uint8_t> f = { CMD_SEND_TXT_MSG, 0 /* TXT_TYPE_PLAIN */, attempt, 0, 0, 0, 0 };
    rdm::companion::put32(&f[3], ts);
    f.insert(f.end(), pub_prefix, pub_prefix + 6);
    f.insert(f.end(), text.begin(), text.end());
    _c.app().send_command(f, [done](const std::vector<uint8_t>& r) {
      SendReply s = { false, false, 0, 0 };
      if (r[0] == RESP_CODE_SENT && r.size() >= 10) {
        s.ok = true;
        s.flood = r[1] == 1;
        s.app_ack = rdm::companion::get32(&r[2]);
        s.est_timeout_ms = rdm::companion::get32(&r[6]);
      }
      done(s);
    });
  }
  void appResetPath(const uint8_t* pub_prefix, Done done) override {
    const SimNode* to = nodeByPrefix(pub_prefix);
    if (!to) return done(false);
    std::vector<uint8_t> f = { CMD_RESET_PATH };
    f.insert(f.end(), to->identity().pub_key, to->identity().pub_key + 32);
    _c.app().send_command(f, [done](const std::vector<uint8_t>& r) { done(r[0] == RESP_CODE_OK); });
  }
  void appSyncNext(SyncDone done) override {
    _c.app().send_command({ CMD_SYNC_NEXT_MESSAGE }, [this, done](const std::vector<uint8_t>& r) {
      Msg m;
      if (r[0] == rdm::companion::RESP_CODE_CONTACT_MSG_RECV_V3 || r[0] == rdm::companion::RESP_CODE_CONTACT_MSG_RECV) {
        size_t i = r[0] == rdm::companion::RESP_CODE_CONTACT_MSG_RECV_V3 ? 4 : 1;   // code, snr, 2 reserved
        memcpy(m.sender_prefix, &r[i], 6);
        m.txt_type = r[i + 7];
        m.ts = rdm::companion::get32(&r[i + 8]);
        m.text.assign(r.begin() + (long)(i + 12), r.end());
        done(true, m);
      } else if (r[0] == RESP_CODE_NO_MORE_MESSAGES || r[0] == RESP_CODE_ERR) {
        done(false, m);
      } else {
        appSyncNext(done);   // a channel message (status channel) is not a DM: keep draining
      }
    });
  }
  void appSetMailbox(const uint8_t* mbx_pub, const uint8_t* k_owner, Done done) override {
    uint8_t f[rdm::companion::SET_MAILBOX_LEN];
    size_t n = rdm::companion::buildSetMailbox(f, mbx_pub, k_owner);
    _c.app().send_command({ f, f + n }, [done](const std::vector<uint8_t>& r) { done(r[0] == RESP_CODE_OK); });
  }
  void appListOutbox(ListDone done) override {
    auto rows = std::make_shared<std::vector<rdm::companion::OutboxRow>>();
    auto count = std::make_shared<int>(-1);
    _c.app().send_command({ rdm::companion::CMD_RDM_LIST_OUTBOX }, [rows, count, done](const std::vector<uint8_t>& r) {
      uint8_t n;
      rdm::companion::OutboxRow row;
      if (rdm::companion::decodeOutboxStart(r.data(), r.size(), n)) {
        *count = n;
      } else if (rdm::companion::decodeOutboxEntry(r.data(), r.size(), row)) {
        rows->push_back(row);
      } else if (r[0] == rdm::companion::RESP_CODE_RDM_OUTBOX_END) {
        EXPECT_EQ(*count, (int)rows->size()) << "LIST_OUTBOX: START count against ENTRY frames";
        done(*rows);
      } else {
        done({});
      }
    });
  }

private:
  // companion protocol codes (examples/companion_radio/MyMesh.cpp, file-local there)
  static const uint8_t CMD_SEND_TXT_MSG = 2, CMD_SYNC_NEXT_MESSAGE = 10, CMD_RESET_PATH = 13;
  static const uint8_t RESP_CODE_OK = 0, RESP_CODE_ERR = 1, RESP_CODE_SENT = 6, RESP_CODE_NO_MORE_MESSAGES = 10;
  static const uint8_t PUSH_CODE_MSG_WAITING = 0x83;

  CompanionNode& _c;
  RdmAppSink* _sink = nullptr;
  size_t _push_seen = 0;

  const SimNode* nodeByPrefix(const uint8_t* prefix) {
    for (size_t i = 0; i < _c.sim().node_count(); i++) {
      if (memcmp(_c.sim().node(i).identity().pub_key, prefix, 6) == 0) return &_c.sim().node(i);
    }
    return nullptr;
  }
};

class SimCompanionCtl : public CompanionCtl {
public:
  explicit SimCompanionCtl(RdmSimCompanion& c) : _c(c), _port(c) {}
  SimNode& node() override { return _c; }
  RdmAppRadio& port() override { return _port; }
  void attach(RdmAppSink& sink) override { _port.attach(sink); }
  void advert() override { _c.advert(); }
  void addContact(const SimNode& other, bool favourite) override { _c.add_contact(other, favourite); }
  bool hasPathTo(const SimNode& other) override { return _c.has_path_to(other); }
  bool isContact(const SimNode& other) override { return _c.knows(other); }
  rdm::Node& rdm() override { return _c.rdm(); }

private:
  RdmSimCompanion& _c;
  CompanionPort _port;
};

class FirmwareCompanionCtl : public CompanionCtl {
public:
  explicit FirmwareCompanionCtl(CompanionNode& c) : _c(c), _port(c) {}
  SimNode& node() override { return _c; }
  RdmAppRadio& port() override { return _port; }
  void attach(RdmAppSink& sink) override { _port.attach(sink); }
  void advert() override { _c.app().advert(true); }
  // CMD_ADD_UPDATE_CONTACT as the app sends it when the user marks a favourite; a known contact keeps its path.
  void addContact(const SimNode& other, bool favourite) override {
    std::vector<uint8_t> f(1 + 32 + 3 + MAX_PATH_SIZE + 32 + 4, 0);
    f[0] = 9;   // CMD_ADD_UPDATE_CONTACT
    memcpy(&f[1], other.identity().pub_key, 32);
    f[33] = ADV_TYPE_CHAT;
    f[34] = favourite ? 0x01 : 0;
    f[35] = OUT_PATH_UNKNOWN;
    ContactInfo* c = _c.powered() ? _c.firmware().lookupContactByPubKey(other.identity().pub_key, 32) : nullptr;
    if (c) {
      f[35] = c->out_path_len;
      memcpy(&f[36], c->out_path, MAX_PATH_SIZE);
    }
    strncpy((char*)&f[36 + MAX_PATH_SIZE], other.name().c_str(), 31);
    _c.app().send_command(f);
  }
  bool hasPathTo(const SimNode& other) override {
    ContactInfo* c = _c.powered() ? _c.firmware().lookupContactByPubKey(other.identity().pub_key, 32) : nullptr;
    return c && c->out_path_len != OUT_PATH_UNKNOWN;
  }
  bool isContact(const SimNode& other) override {
    return _c.powered() && _c.firmware().lookupContactByPubKey(other.identity().pub_key, 32) != nullptr;
  }
  rdm::Node& rdm() override { return _c.firmware().rdm(); }

private:
  CompanionNode& _c;
  FramePort _port;
};

enum class Layout : uint8_t {
  VIA_REPEATER,   // B-R-A, mailboxes at R: the behaviour topology of 06 par. 6.2
  DIRECT          // every pair linked (minus B-A when forbidden): the airtime topology, one hop per packet
};

enum class CompanionModel : uint8_t {
  SIM,        // RdmSimCompanion (WP5): RdmChatMesh with the app side as direct calls
  FIRMWARE    // CompanionNode: the real examples/companion_radio MyMesh (WP6), app over companion frames
};

struct WorldOptions {
  Layout   layout = Layout::VIA_REPEATER;
  CompanionModel model = CompanionModel::SIM;
  int      mailboxes = 0;          // 0, 1 or 2
  bool     real_mbxd = false;      // mailbox 0 runs mbxd through SubprocessBackend
  bool     ab_link = true;         // DIRECT: link B-A (06 S07: no direct link B-A)
  bool     warmup = true;          // one delivered DM first: paths, cap and (with a mailbox) MBX_INFO known
  bool     bob_rdm_client = true;  // false: Bob's app is the standard app (no CMD_RDM_ENABLE)
  uint32_t alice_rdm_flash = 0;    // > 0: Alice's flash has only this much free for RDM (fewer register slots)
  uint64_t seed = 42;
};

class World {
public:
  Simulator s;
  SimNode* bob = nullptr;
  SimNode* alice = nullptr;
  RepeaterNode* rep = nullptr;
  std::vector<SimNode*> mbx;
  std::unique_ptr<CompanionCtl> bob_ctl, alice_ctl;
  std::unique_ptr<RdmSimApp> bob_app, alice_app;
  WireLog wires{s};
  uint8_t k_owner[2][16];
  std::unique_ptr<SubprocessBackend> mbxd;
  std::string mbxd_dir;

  explicit World(const WorldOptions& o);
  ~World();

  RdmSimApp& bobApp() { return *bob_app; }
  RdmSimApp& aliceApp() { return *alice_app; }
  CompanionCtl& bobCtl() { return *bob_ctl; }
  CompanionCtl& aliceCtl() { return *alice_ctl; }
  // Sends like bobApp().send() and runs until the radio answered (RESP_CODE_SENT), so app_acks[0] is known.
  int bobSends(const std::string& text, AppRetry policy = AppRetry{1, false});

  // Simulator time with the apps ticking between steps.
  void runFor(uint64_t ms);
  bool runUntil(const std::function<bool()>& pred, uint64_t timeout_ms);
  void tickApps();

  // Power cycles through the app: the link drops with the radio; connect() again after power_on() if wanted.
  void powerOff(SimNode& n) { n.power_off(); tickApps(); }
  void powerOn(SimNode& n) { n.power_on(); tickApps(); }

  MemMailboxBackend& memBackend(int i) { return static_cast<RdmSimMailbox*>(mbx.at((size_t)i))->backend(); }
  const uint8_t* mbxPub(int i) { return mbx.at((size_t)i)->identity().pub_key; }
  // Registers Alice as owner at mailbox i and reboots it, so the radio loads the ACL (06 par. 3.16).
  void addAliceAsOwner(int i);
  bool bobKnowsAliceMailbox(int i);
  // Bob registered at mailbox i before (MemMailboxBackend), and M rebooted so its peer cache has him from the ACL.
  void registerBobAt(int i);
  // Wipes Alice's inbox and register as a corrupt or reformatted file would (03 par. 6, scenario 6); the
  // next boot recreates them with a new store_id. Alice must be powered off.
  void wipeAliceInbox();

  // State of c's outbox entry for (to, ts), read from the radio; -1 if there is none (never made, or freed).
  int outState(CompanionCtl& c, const SimNode& to, uint32_t ts);
  bool bobState(int bob_msg, rdm::OutState st) { return outState(*bob_ctl, *alice, bob_app->sent(bob_msg).ts) == (int)st; }
  // Runtime switch to upstream behaviour (scenario 10); lasts until the next boot.
  void setAliceRdm(bool on) {
    Simulator::OnNode ctx(s, *alice);
    alice_ctl->rdm().setEnabled(on);
  }

  // Bob's 0x91 states and Alice's display for one message, for diagnostics.
  std::string trace(int bob_msg);

  WorldOptions opt;

private:
  void introduce();
};

inline std::vector<rdm::UserStatus> S(std::initializer_list<rdm::UserStatus> l) { return l; }

}
