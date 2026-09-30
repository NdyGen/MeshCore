#pragma once

// Phone app of an RDM companion in the simulator (06 par. 6.2): the standard app behaviour (retries on timeout,
// sync on PUSH_CODE_MSG_WAITING) plus the fork frames of 06 par. 3.14, over companion frames to the real MyMesh in
// CompanionNode, with a log of everything it received. Also the white-box view a test takes on a companion through
// its app's push log and its firmware: 0x91 statuses, SEND_CONFIRMED and CONTACT_DELETED pushes, the contact list
// and the outbox.

#include <gtest/gtest.h>

#include <ChatNode.h>
#include <CompanionFrames.h>
#include <CompanionNode.h>
#include <RdmCompanionProto.h>
#include <Sim.h>
#include <helpers/rdm/RdmConfig.h>
#include <helpers/rdm/RdmTypes.h>

#include <stdint.h>
#include <string.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace sim {

// ---- white-box view on a companion ----

// 0x91 statuses pushed for app_ack, in arrival order.
inline std::vector<rdm::UserStatus> statusesFor(const CompanionApp& app, uint32_t app_ack) {
  std::vector<rdm::UserStatus> out;
  for (const auto& f : app.push_log()) {
    rdm::companion::StatusPush p;
    if (rdm::companion::decodeStatus(f.data(), f.size(), p) && p.app_ack == app_ack) out.push_back(p.status);
  }
  return out;
}

inline bool hasStatus(const CompanionApp& app, uint32_t app_ack, rdm::UserStatus s) {
  for (rdm::UserStatus x : statusesFor(app, app_ack)) {
    if (x == s) return true;
  }
  return false;
}

// app_acks of the PUSH_CODE_SEND_CONFIRMED frames, in arrival order.
inline std::vector<uint32_t> confirmsOf(const CompanionApp& app) {
  std::vector<uint32_t> out;
  for (const auto& f : app.push_log()) {
    if (f.size() >= 5 && f[0] == frames::PUSH_CODE_SEND_CONFIRMED) out.push_back(rdm::get32(&f[1]));
  }
  return out;
}

// K3: pubkeys of contacts the firmware removed because they became hidden peers (PUSH_CODE_CONTACT_DELETED), in order.
inline std::vector<std::vector<uint8_t>> removedContacts(const CompanionApp& app) {
  std::vector<std::vector<uint8_t>> out;
  for (const auto& f : app.push_log()) {
    if (f.size() >= 1 + PUB_KEY_SIZE && f[0] == frames::PUSH_CODE_CONTACT_DELETED) {
      out.emplace_back(f.begin() + 1, f.begin() + 1 + PUB_KEY_SIZE);
    }
  }
  return out;
}

inline ContactInfo* contactOf(CompanionNode& c, const SimNode& other) {
  return c.powered() ? c.firmware().lookupContactByPubKey(other.identity().pub_key, PUB_KEY_SIZE) : nullptr;
}
inline bool isContact(CompanionNode& c, const SimNode& other) { return contactOf(c, other) != nullptr; }
inline bool hasPathTo(CompanionNode& c, const SimNode& other) {
  ContactInfo* ci = contactOf(c, other);
  return ci && ci->out_path_len != OUT_PATH_UNKNOWN;
}
inline int contactCount(CompanionNode& c) {
  int n = 0;
  ContactInfo ci;
  for (int i = 0; c.powered() && i < c.firmware().getTotalContactSlots(); i++) {
    if (c.firmware().getContactByIdx(i, ci) && ci.type != ADV_TYPE_NONE) n++;
  }
  return n;
}

// State of c's outbox entry for (to, ts), read from the radio; -1 if there is none (never made, or freed).
inline int outState(CompanionNode& c, const SimNode& to, uint32_t ts) {
  if (!c.powered()) return -1;
  Simulator::OnNode ctx(c.sim(), c);
  rdm::OutEntry e[RDM_OUTBOX_SLOTS_MAX];
  uint8_t n = c.firmware().rdm().listOutbox(e, RDM_OUTBOX_SLOTS_MAX);
  for (uint8_t i = 0; i < n; i++) {
    if (e[i].ts == ts && memcmp(e[i].pub_prefix, to.identity().pub_key, 6) == 0) return (int)e[i].state;
  }
  return -1;
}

// ---- commands a test sends outside the app model ----

// CMD_ADD_UPDATE_CONTACT as the app sends it when the user marks a favourite; a known contact keeps its path.
inline std::vector<uint8_t> addContactFrame(CompanionNode& c, const SimNode& other, bool favourite) {
  std::vector<uint8_t> f(1 + 32 + 3 + MAX_PATH_SIZE + 32 + 4, 0);
  f[0] = frames::CMD_ADD_UPDATE_CONTACT;
  memcpy(&f[1], other.identity().pub_key, 32);
  f[33] = ADV_TYPE_CHAT;
  f[34] = favourite ? 0x01 : 0;
  f[35] = OUT_PATH_UNKNOWN;
  if (ContactInfo* ci = contactOf(c, other)) {
    f[35] = ci->out_path_len;
    memcpy(&f[36], ci->out_path, MAX_PATH_SIZE);
  }
  strncpy((char*)&f[36 + MAX_PATH_SIZE], other.name().c_str(), 31);
  return f;
}

// Queues the command; the reply comes as the simulator runs.
inline void addContact(CompanionNode& c, const SimNode& other, bool favourite) {
  c.app().send_command(addContactFrame(c, other, favourite));
}

// Sends one command and runs the simulator until its reply; true on RESP_CODE_OK. For tests without an app model
// (test_rdm_sim): with an RdmSimApp, go through it so its ticks are not skipped.
inline bool commandOk(CompanionNode& c, const std::vector<uint8_t>& frame, uint64_t timeout_ms = 5000) {
  bool got = false, ok = false;
  c.app().send_command(frame, [&](const std::vector<uint8_t>& r) {
    ok = !r.empty() && r[0] == frames::RESP_CODE_OK;
    got = true;
  });
  c.sim().run_until([&] { return got; }, timeout_ms);
  return got && ok;
}

// CMD_RDM_ENABLE (protocol version 1): 0x91 pushes on this connection from here on.
inline bool enableRdm(CompanionNode& c) {
  uint8_t f[2];
  return commandOk(c, std::vector<uint8_t>(f, f + rdm::companion::buildEnable(f)));
}

// CMD_RDM_SET_MAILBOX with the keys, or a revoke with nullptr.
inline bool setMailbox(CompanionNode& c, const uint8_t* mbx_pub, const uint8_t* k_owner) {
  uint8_t f[rdm::companion::SET_MAILBOX_LEN];
  return commandOk(c, std::vector<uint8_t>(f, f + rdm::companion::buildSetMailbox(f, mbx_pub, k_owner)));
}

// ---- the app model ----

class RdmSimApp;

// Radio side of the companion link as the app uses it: one call per companion command, answered through a callback
// as the reply arrives over the frames, through CompanionApp's command queue (one command in flight, multi-frame
// replies up to their closing frame). Pushes come from CompanionApp's push log.
class FramePort {
public:
  struct SendReply {            // RESP_CODE_SENT, or an error frame (ok = false)
    bool     ok;
    bool     flood;
    uint32_t app_ack;           // expected_ack: ACK_R of this attempt
    uint32_t est_timeout_ms;
  };
  using Done     = std::function<void(bool ok)>;
  using SendDone = std::function<void(const SendReply&)>;
  using SyncDone = std::function<void(bool has_msg, const frames::ContactMsg&)>;   // false: RESP_CODE_NO_MORE_MESSAGES
  using ListDone = std::function<void(const std::vector<rdm::companion::OutboxRow>&)>;

  explicit FramePort(CompanionNode& c) : _c(c) {}
  void attach(RdmSimApp& sink) { _sink = &sink; }
  CompanionNode& node() { return _c; }

  inline void appPoll();   // hand pushes that arrived to the app
  bool appLinkUp() { return _c.app().connected(); }   // false once a radio reset dropped the link
  // Link up, CMD_DEVICE_QUERY, CMD_APP_START, CMD_SET_DEVICE_TIME with the phone clock and, for an RDM client,
  // CMD_RDM_ENABLE (version 1).
  void appConnect(bool rdm_enable) {
    _c.app().auto_sync = false;   // RdmSimApp syncs
    _c.app().connect();
    if (!rdm_enable) return;
    uint8_t f[2];
    _c.app().send_command({ f, f + rdm::companion::buildEnable(f) });
  }
  void appDisconnect() { _c.app().disconnect(); }
  void appSendText(const uint8_t pub_prefix[6], uint32_t ts, uint8_t attempt, const std::string& text, SendDone done) {
    _c.app().send_command(frames::buildSendTxt(pub_prefix, ts, attempt, text), [done](const std::vector<uint8_t>& r) {
      SendReply s = { false, false, 0, 0 };
      frames::SentReply reply;
      if (frames::decodeSent(r, reply)) {
        s.ok = true;
        s.flood = reply.flood;
        s.app_ack = reply.expected_ack;
        s.est_timeout_ms = reply.est_timeout_ms;
      }
      done(s);
    });
  }
  void appResetPath(const uint8_t pub_prefix[6], Done done) {
    const SimNode* to = _c.sim().nodeByPrefix(pub_prefix, 6);
    if (!to) return done(false);
    std::vector<uint8_t> f = { frames::CMD_RESET_PATH };
    f.insert(f.end(), to->identity().pub_key, to->identity().pub_key + PUB_KEY_SIZE);
    _c.app().send_command(f, [done](const std::vector<uint8_t>& r) { done(r[0] == frames::RESP_CODE_OK); });
  }
  void appSyncNext(SyncDone done) {
    _c.app().send_command({ frames::CMD_SYNC_NEXT_MESSAGE }, [this, done](const std::vector<uint8_t>& r) {
      frames::ContactMsg m;
      if (frames::decodeContactMsg(r, m)) {
        done(true, m);
      } else if (r[0] == frames::RESP_CODE_NO_MORE_MESSAGES || r[0] == frames::RESP_CODE_ERR) {
        done(false, m);
      } else {
        appSyncNext(done);   // a channel message (status channel) is not a DM: keep draining
      }
    });
  }
  void appSetMailbox(const uint8_t* mbx_pub, const uint8_t* k_owner, Done done) {
    uint8_t f[rdm::companion::SET_MAILBOX_LEN];
    size_t n = rdm::companion::buildSetMailbox(f, mbx_pub, k_owner);
    _c.app().send_command({ f, f + n }, [done](const std::vector<uint8_t>& r) { done(r[0] == frames::RESP_CODE_OK); });
  }
  // CMD_RDM_LIST_OUTBOX: START | ENTRY per entry | END
  void appListOutbox(ListDone done) {
    auto rows = std::make_shared<std::vector<rdm::companion::OutboxRow>>();
    auto count = std::make_shared<int>(-1);
    _c.app().send_command({ frames::CMD_RDM_LIST_OUTBOX }, [rows, count, done](const std::vector<uint8_t>& r) {
      uint8_t n;
      rdm::companion::OutboxRow row;
      if (rdm::companion::decodeOutboxStart(r.data(), r.size(), n)) {
        *count = n;
      } else if (rdm::companion::decodeOutboxEntry(r.data(), r.size(), row)) {
        rows->push_back(row);
      } else if (r[0] == frames::RESP_CODE_RDM_OUTBOX_END) {
        EXPECT_EQ(*count, (int)rows->size()) << "LIST_OUTBOX: START count against ENTRY frames";
        done(*rows);
      } else {
        done({});
      }
    }, frames::RESP_CODE_RDM_OUTBOX_END);
  }

private:
  CompanionNode& _c;
  RdmSimApp* _sink = nullptr;
  size_t _push_seen = 0;
};

// Driven by tick(), called between simulator steps; nextDeadlineMs() is its next timer (the standard app's retry).
class RdmSimApp {
public:
  struct StatusPush {
    uint64_t        t_ms;
    uint32_t        app_ack;
    rdm::UserStatus status;
    uint8_t         key[4];
    uint32_t        ts;
    uint8_t         prefix[6];
  };
  struct ConfirmPush { uint64_t t_ms; uint32_t app_ack; };
  struct Msg {
    uint64_t    t_ms;
    uint8_t     sender_prefix[6];
    uint32_t    ts;
    uint8_t     txt_type;
    std::string text;
  };
  struct Sent {
    int         id;
    uint8_t     to[6];
    std::string text;
    uint32_t    ts;
    AppRetry    policy;
    std::vector<uint8_t>  attempts;
    std::vector<uint32_t> app_acks;
    std::vector<uint64_t> sent_at;      // when the RESP_CODE_SENT came
    std::vector<bool>     accepted;     // RESP_CODE_SENT (true) or error frame
    uint32_t    est_timeout_ms = 0;
    bool        confirmed = false;      // standard app: tick
    uint64_t    confirmed_at = 0;
    bool        failed = false;         // standard app: "failed" after the last attempt timed out
  };

  RdmSimApp(Simulator& sim, CompanionNode& radio) : _sim(sim), _radio(radio) { _radio.attach(*this); }

  bool rdm_client = true;   // sends CMD_RDM_ENABLE after connecting (the standard app does not)
  bool auto_sync  = true;   // syncs on connect and on PUSH_CODE_MSG_WAITING

  bool connected() const { return _connected; }

  void connect() {
    if (_connected) return;
    _connected = true;
    ++_link_gen;
    _radio.appConnect(rdm_client);
    if (auto_sync) _sync_queued = true;
  }
  void disconnect() {
    if (!_connected) return;
    dropLink();
    _radio.appDisconnect();
  }

  // CMD_SEND_TXT_MSG with the phone clock as timestamp. Default: one attempt; pass AppRetry() for the standard
  // app's retries (attempt + 1 after each est_timeout, path reset before the last). -1 if not connected.
  int send(const SimNode& to, const std::string& text, AppRetry policy = AppRetry{1, false}) {
    if (!_connected) return -1;
    Sent m;
    m.id = (int)_sent.size();
    memcpy(m.to, to.identity().pub_key, 6);
    m.text = text;
    m.ts = _sim.wall_epoch();
    m.policy = policy;
    _sent.push_back(m);
    transmit(m.id, 0);
    return m.id;
  }
  bool answered(int id) const { return !sent(id).accepted.empty() && sent(id).accepted.size() == sent(id).attempts.size(); }

  void sync() { if (_connected) _sync_queued = true; }
  // CMD_RDM_SET_MAILBOX; mailboxResult(): -1 pending, 0 refused, 1 accepted.
  void setMailbox(const uint8_t* mbx_pub, const uint8_t* k_owner) {
    if (!_connected) return;
    _mailbox_result = -1;
    _radio.appSetMailbox(mbx_pub, k_owner, [this](bool ok) { _mailbox_result = ok ? 1 : 0; });
  }
  int mailboxResult() const { return _mailbox_result; }
  // CMD_RDM_LIST_OUTBOX; outboxListings() counts the complete answers (END frame), outbox() is the latest.
  void requestOutbox() {
    if (!_connected) return;
    _radio.appListOutbox([this](const std::vector<rdm::companion::OutboxRow>& rows) {
      _outbox = rows;
      _outbox_listings++;
    });
  }
  size_t outboxListings() const { return _outbox_listings; }
  const std::vector<rdm::companion::OutboxRow>& outbox() const { return _outbox; }

  // pushes from the radio (FramePort::appPoll); the app records whatever arrives, so a push to the wrong app shows
  // up in the assertions
  void pushRdmStatus(const uint8_t pub_prefix[6], uint32_t app_ack, rdm::UserStatus s, const uint8_t key[4], uint32_t ts) {
    StatusPush p;
    p.t_ms = _sim.now();
    p.app_ack = app_ack;
    p.status = s;
    memcpy(p.key, key, 4);
    p.ts = ts;
    memcpy(p.prefix, pub_prefix, 6);
    _status.push_back(p);
  }
  void pushSendConfirmed(uint32_t app_ack) {
    _confirms.push_back({ _sim.now(), app_ack });
    for (auto& m : _sent) {
      for (uint32_t a : m.app_acks) {
        if (a == app_ack && !m.confirmed) {
          m.confirmed = true;
          m.confirmed_at = _sim.now();
        }
      }
    }
  }
  void pushMsgWaiting() {
    if (auto_sync && _connected) _sync_queued = true;
  }

  void tick() {
    _radio.appPoll();
    if (_connected && !_radio.appLinkUp()) dropLink();   // the radio reset: the phone link is gone
    if (!_connected) return;
    if (_sync_queued && !_syncing) {
      _sync_queued = false;
      _syncing = true;
      syncNext(_link_gen);
    }
    for (auto& m : _sent) {
      if (m.confirmed || m.failed || !answered(m.id) || _sim.now() < deadline(m)) continue;
      if ((int)m.attempts.size() >= m.policy.max_attempts) {
        m.failed = true;
        continue;
      }
      if (m.policy.flood_on_last && (int)m.attempts.size() + 1 == m.policy.max_attempts) {
        _radio.appResetPath(m.to, [](bool) {});
      }
      transmit(m.id, (uint8_t)m.attempts.size());
    }
  }
  bool busy() const { return _connected && (_sync_queued || _syncing); }
  uint64_t nextDeadlineMs() const {
    uint64_t t = UINT64_MAX;
    if (!_connected) return t;
    for (const auto& m : _sent) {
      if (!m.confirmed && !m.failed && answered(m.id) && deadline(m) < t) t = deadline(m);
    }
    return t;
  }

  // ---- observations ----
  const Sent& sent(int id) const { return _sent.at((size_t)id); }
  const std::vector<StatusPush>& statusLog() const { return _status; }
  const std::vector<Msg>& inbox() const { return _inbox; }

  // 0x91 states for message `id` in arrival order (matched on contact and timestamp: stable over app retries).
  std::vector<rdm::UserStatus> statuses(int id) const {
    std::vector<rdm::UserStatus> v;
    for (const auto& p : statusPushesFor(id)) v.push_back(p.status);
    return v;
  }
  std::vector<StatusPush> statusPushesFor(int id) const {
    const Sent& m = sent(id);
    std::vector<StatusPush> v;
    for (const auto& p : _status) {
      if (p.ts == m.ts && memcmp(p.prefix, m.to, 6) == 0) v.push_back(p);
    }
    return v;
  }
  bool hasStatus(int id, rdm::UserStatus s) const { return firstStatusAt(id, s) != UINT64_MAX; }
  uint64_t firstStatusAt(int id, rdm::UserStatus s) const {
    for (const auto& p : statusPushesFor(id)) {
      if (p.status == s) return p.t_ms;
    }
    return UINT64_MAX;
  }
  // PUSH_CODE_SEND_CONFIRMED frames carrying one of this message's app_acks.
  size_t confirmCount(int id) const {
    const Sent& m = sent(id);
    size_t n = 0;
    for (const auto& c : _confirms) {
      for (uint32_t a : m.app_acks) {
        if (a == c.app_ack) {
          n++;
          break;
        }
      }
    }
    return n;
  }
  size_t inboxCount(const std::string& text) const { return countText(_inbox, text); }

private:
  Simulator& _sim;
  FramePort _radio;
  bool _connected = false;
  bool _sync_queued = false;
  bool _syncing = false;
  unsigned _link_gen = 0;      // callbacks of an older connection are ignored
  int _mailbox_result = -1;
  size_t _outbox_listings = 0;
  std::vector<rdm::companion::OutboxRow> _outbox;
  std::vector<Sent> _sent;
  std::vector<StatusPush> _status;
  std::vector<ConfirmPush> _confirms;
  std::vector<Msg> _inbox;

  static uint64_t deadline(const Sent& m) { return m.sent_at.back() + (m.est_timeout_ms ? m.est_timeout_ms : 1); }

  void dropLink() {
    _connected = false;
    _sync_queued = false;
    _syncing = false;
    ++_link_gen;
  }
  // CMD_SYNC_NEXT_MESSAGE until NO_MORE_MESSAGES: the request after a message is what marks it synced.
  void syncNext(unsigned gen) {
    _radio.appSyncNext([this, gen](bool has, const frames::ContactMsg& m) {
      if (gen != _link_gen) return;
      if (!has) {
        _syncing = false;
        return;
      }
      Msg in;
      in.t_ms = _sim.now();
      memcpy(in.sender_prefix, m.sender_prefix, 6);
      in.ts = m.ts;
      in.txt_type = m.txt_type;
      in.text = m.text;
      _inbox.push_back(in);
      syncNext(gen);
    });
  }
  void transmit(int id, uint8_t attempt) {
    Sent& m = _sent[(size_t)id];
    m.attempts.push_back(attempt);
    _radio.appSendText(m.to, m.ts, attempt, m.text, [this, id](const FramePort::SendReply& r) {
      Sent& s = _sent[(size_t)id];
      s.sent_at.push_back(_sim.now());
      s.accepted.push_back(r.ok);
      if (r.ok) {
        s.app_acks.push_back(r.app_ack);
        s.est_timeout_ms = r.est_timeout_ms;
      } else {
        s.failed = true;
      }
    });
  }
};

inline void FramePort::appPoll() {
  const auto& log = _c.app().push_log();
  for (; _push_seen < log.size(); _push_seen++) {
    const std::vector<uint8_t>& f = log[_push_seen];
    if (!_sink || f.empty()) continue;
    rdm::companion::StatusPush p;
    if (rdm::companion::decodeStatus(f.data(), f.size(), p)) {
      _sink->pushRdmStatus(p.pub_prefix, p.app_ack, p.status, p.key, p.ts);
    } else if (f[0] == frames::PUSH_CODE_SEND_CONFIRMED && f.size() >= 5) {
      _sink->pushSendConfirmed(rdm::get32(&f[1]));
    } else if (f[0] == frames::PUSH_CODE_MSG_WAITING) {
      _sink->pushMsgWaiting();
    }
  }
}

}
