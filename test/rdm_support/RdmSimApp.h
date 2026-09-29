#pragma once

// Phone app of an RDM companion in the simulator (06 par. 6.2): the standard app behaviour (retries on timeout,
// sync on PUSH_CODE_MSG_WAITING) plus the fork frames of 06 par. 3.14, with a log of everything it received.
// The app talks to its radio through RdmAppRadio; the radio pushes back through RdmAppSink.

#include <ChatNode.h>
#include <RdmCompanionProto.h>
#include <Sim.h>
#include <helpers/rdm/RdmTypes.h>

#include <stdint.h>
#include <string.h>

#include <functional>
#include <string>
#include <vector>

namespace sim {

// Radio side of the companion link as the app uses it: one call per companion command, answered through a callback
// as the reply arrives (at once for a radio model without a link, one loop later over companion frames). Calls
// come from outside the node's loop; an implementation runs them under Simulator::OnNode for its own node.
class RdmAppRadio {
public:
  struct SendReply {            // RESP_CODE_SENT, or an error frame (ok = false)
    bool     ok;
    bool     flood;
    uint32_t app_ack;           // expected_ack: ACK_R of this attempt
    uint32_t est_timeout_ms;
  };
  struct Msg {                  // RESP_CODE_CONTACT_MSG_RECV_V3
    uint8_t     sender_prefix[6];
    uint32_t    ts;
    uint8_t     txt_type;
    std::string text;
  };
  using Done     = std::function<void(bool ok)>;
  using SendDone = std::function<void(const SendReply&)>;
  using SyncDone = std::function<void(bool has_msg, const Msg&)>;   // false: RESP_CODE_NO_MORE_MESSAGES
  using ListDone = std::function<void(const std::vector<rdm::companion::OutboxRow>&)>;

  virtual ~RdmAppRadio() {}
  // Link up, CMD_APP_START, CMD_SET_DEVICE_TIME(phone_time) and, for an RDM client, CMD_RDM_ENABLE (version 1);
  // done(true) when the radio accepted CMD_RDM_ENABLE.
  virtual void appConnect(uint32_t phone_time, bool rdm_enable, Done done) = 0;
  virtual void appDisconnect() = 0;
  virtual bool appLinkUp() = 0;   // false once a radio reset dropped the link
  virtual void appPoll() = 0;     // hand pushes that arrived to the sink
  virtual void appSendText(const uint8_t pub_prefix[6], uint32_t ts, uint8_t attempt, const std::string& text,
                           SendDone done) = 0;                                    // CMD_SEND_TXT_MSG
  virtual void appResetPath(const uint8_t pub_prefix[6], Done done) = 0;          // CMD_RESET_PATH
  virtual void appSyncNext(SyncDone done) = 0;                                    // CMD_SYNC_NEXT_MESSAGE
  virtual void appSetMailbox(const uint8_t* mbx_pub, const uint8_t* k_owner, Done done) = 0;   // CMD_RDM_SET_MAILBOX
  virtual void appListOutbox(ListDone done) = 0;   // CMD_RDM_LIST_OUTBOX: START | ENTRY per entry | END
};

// Pushes from the radio. The radio decides what to push (0x91 only after CMD_RDM_ENABLE on this connection);
// the app records whatever arrives, so a push to the wrong app shows up in the assertions.
class RdmAppSink {
public:
  virtual ~RdmAppSink() {}
  virtual void pushRdmStatus(const uint8_t pub_prefix[6], uint32_t app_ack, rdm::UserStatus s, const uint8_t key[4],
                             uint32_t ts) = 0;                   // PUSH_CODE_RDM_STATUS 0x91
  virtual void pushSendConfirmed(uint32_t app_ack) = 0;          // PUSH_CODE_SEND_CONFIRMED
  virtual void pushMsgWaiting() = 0;                             // PUSH_CODE_MSG_WAITING
};

// Driven by tick(), called between simulator steps; nextDeadlineMs() is its next timer (the standard app's retry).
class RdmSimApp : public RdmAppSink {
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

  RdmSimApp(Simulator& sim, RdmAppRadio& radio) : _sim(sim), _radio(radio) {}

  bool rdm_client = true;   // sends CMD_RDM_ENABLE after connecting (the standard app does not)
  bool auto_sync  = true;   // syncs on connect and on PUSH_CODE_MSG_WAITING

  bool connected() const { return _connected; }
  bool rdmEnabled() const { return _connected && _rdm_enabled; }

  void connect() {
    if (_connected) return;
    _connected = true;
    _rdm_enabled = false;
    unsigned gen = ++_link_gen;
    _radio.appConnect(_sim.wall_epoch(), rdm_client, [this, gen](bool en) {
      if (gen == _link_gen) _rdm_enabled = en;
    });
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

  // RdmAppSink
  void pushRdmStatus(const uint8_t pub_prefix[6], uint32_t app_ack, rdm::UserStatus s, const uint8_t key[4],
                     uint32_t ts) override {
    StatusPush p;
    p.t_ms = _sim.now();
    p.app_ack = app_ack;
    p.status = s;
    memcpy(p.key, key, 4);
    p.ts = ts;
    memcpy(p.prefix, pub_prefix, 6);
    _status.push_back(p);
  }
  void pushSendConfirmed(uint32_t app_ack) override {
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
  void pushMsgWaiting() override {
    _msg_waiting++;
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
  const std::vector<ConfirmPush>& confirmLog() const { return _confirms; }
  const std::vector<Msg>& inbox() const { return _inbox; }
  uint32_t msgWaitingCount() const { return _msg_waiting; }

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
  size_t inboxCount(const std::string& text) const {
    size_t n = 0;
    for (const auto& m : _inbox) n += m.text == text;
    return n;
  }

private:
  Simulator&   _sim;
  RdmAppRadio& _radio;
  bool _connected = false;
  bool _rdm_enabled = false;
  bool _sync_queued = false;
  bool _syncing = false;
  unsigned _link_gen = 0;      // callbacks of an older connection are ignored
  int _mailbox_result = -1;
  uint32_t _msg_waiting = 0;
  size_t _outbox_listings = 0;
  std::vector<rdm::companion::OutboxRow> _outbox;
  std::vector<Sent> _sent;
  std::vector<StatusPush> _status;
  std::vector<ConfirmPush> _confirms;
  std::vector<Msg> _inbox;

  static uint64_t deadline(const Sent& m) { return m.sent_at.back() + (m.est_timeout_ms ? m.est_timeout_ms : 1); }

  void dropLink() {
    _connected = false;
    _rdm_enabled = false;
    _sync_queued = false;
    _syncing = false;
    ++_link_gen;
  }
  // CMD_SYNC_NEXT_MESSAGE until NO_MORE_MESSAGES: the request after a message is what marks it synced.
  void syncNext(unsigned gen) {
    _radio.appSyncNext([this, gen](bool has, const RdmAppRadio::Msg& m) {
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
    _radio.appSendText(m.to, m.ts, attempt, m.text, [this, id](const RdmAppRadio::SendReply& r) {
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

}
