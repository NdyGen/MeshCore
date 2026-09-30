#pragma once

#include <helpers/rdm/mailbox/MailboxCore.h>

#include "TestUtil.h"

#include <deque>
#include <functional>
#include <vector>

// In-memory rdm::MailboxBackend for unit tests and the simulator. Implements the same rules as meshcore-mailboxd
// (examples/mailbox_server/meshcore-mailboxd/README.md); both run test/rdm_vectors/mailbox_conformance.json.
// Replies are queued synchronously and handed out by poll() in request order.
class MemMailboxBackend : public rdm::MailboxBackend {
public:
  enum class Mode : uint8_t {
    NORMAL,
    NO_REPLY,   // Pi absent: requests are dropped without a reply or state change (scenario 9a)
    WITHHOLD    // FETCH never carries a copy and reports remaining 0 (scenario 9b)
  };
  // A malicious M (R5): STATUS says DELIVERED for every message it has not seen synced, with the ACK_R from Alice's
  // report (the best M knows) or with random bytes. Everything else stays honest.
  enum class Lie : uint8_t { NONE, DELIVERED_WITH_ACK_R, DELIVERED_WITH_RANDOM };
  enum class MsgState : uint8_t { STORED, SENT, ON_RADIO, DELIVERED, REJECTED, EXPIRED, SYNC_EXPIRED };

  static const uint32_t DAY_S    = 86400;
  static const uint32_t RETAIN_S = 30 * DAY_S;
  static const uint8_t  ACL_MAX  = 64;

  bool store(uint32_t id, const uint8_t owner[4], const uint8_t sender_pub[32], const uint8_t pkt_hash[8],
             const uint8_t* payload, uint8_t len) override;
  bool reg(uint32_t id, const uint8_t sender_pub[32], const uint8_t owner[4], const uint8_t token[8]) override;
  bool fetch(uint32_t id, const uint8_t client_pub[32], uint8_t flags, uint32_t store_id, const rdm::Report* r,
             uint8_t n) override;
  bool stat(uint32_t id, const uint8_t sender_pub[32], const uint8_t (*hashes)[8], uint8_t n) override;
  bool poll(rdm::BackendReply& out) override;
  bool pollAcl(uint8_t pub_out[32], bool& is_owner, uint8_t owner_out[4]) override;
  bool ready() override { return _ready; }
  uint32_t backendTime() override { return now(); }   // the Pi clock; 0 until setTime/setTimeSource

  // Pi side (the daemon CLI: owner-add / deny)
  bool addOwner(const uint8_t pub[32], const uint8_t k_owner[16], uint8_t ttl_days = 7, uint8_t sync_days = 30,
                uint8_t quota = 20);
  bool deny(const uint8_t owner[4], const uint8_t pub[32]);
  void hello();       // radio boot: queue the ACL (owners first, then recent depositors, max 64) and set ready
  void advance();     // run the timers at the current time

  void setTime(uint32_t now) { _now = now; }
  void setTimeSource(std::function<uint32_t()> fn) { _time_fn = fn; }
  void setMode(Mode m) { _mode = m; }
  void setLie(Lie l) { _lie = l; }
  void setReady(bool r) { _ready = r; }

  bool     messageState(const uint8_t pkt_hash[8], MsgState& out) const;
  size_t   messageCount() const { return _msgs.size(); }
  size_t   pendingReplies() const { return _replies.size(); }
  uint32_t requestCount() const { return _requests; }

private:
  struct Owner {
    uint8_t  pub[32];
    uint8_t  k_owner[16];
    uint8_t  ttl_days, sync_days, quota;
    bool     has_store_id;
    uint32_t store_id;
    bool     has_last_sender;
    uint8_t  last_sender[32];   // sender of the previous copy handed out; senders take turns in pubkey order
  };
  struct Depositor {
    uint8_t  pub[32];
    uint8_t  owner[4];
    uint32_t last_seen;
    uint32_t seq;
  };
  struct Denied {
    uint8_t owner[4];
    uint8_t pub[32];
  };
  struct Msg {
    uint8_t  pkt_hash[8];
    uint8_t  owner[4];
    uint8_t  sender[32];
    std::vector<uint8_t> payload;
    uint32_t created, t_radio, t_sync, final_at;
    bool     has_final;
    MsgState state;
    uint8_t  ack_r[6], ack_s[6];
  };
  struct AclLine {
    uint8_t pub[32];
    bool    is_owner;
    uint8_t owner[4];
  };

  std::vector<Owner>     _owners;
  std::vector<Depositor> _acl;
  std::vector<Denied>    _deny;
  std::vector<Msg>       _msgs;     // insertion order = oldest first
  std::deque<rdm::BackendReply> _replies;
  std::deque<AclLine>    _acl_out;
  std::function<uint32_t()> _time_fn;
  uint32_t _now = 0;
  uint32_t _acl_seq = 0;
  uint32_t _requests = 0;
  Mode     _mode = Mode::NORMAL;
  Lie      _lie = Lie::NONE;
  Lcg32    _lie_rng{0x9E3779B9u};
  bool     _ready = true;

  uint32_t   now() const { return _time_fn ? _time_fn() : _now; }
  bool       accept();
  void       housekeeping(uint32_t now);
  Owner*     ownerBy4(const uint8_t owner[4]);
  Owner*     ownerByPub(const uint8_t pub[32]);
  Depositor* depositor(const uint8_t owner[4], const uint8_t pub[32]);
  bool       isDenied(const uint8_t owner[4], const uint8_t pub[32]) const;
  Msg*       findMsg(const uint8_t pkt_hash[8]);
  Msg*       nextCopy(Owner& o);
  void       applyReport(uint32_t now, const Owner& o, const rdm::Report& r);
  rdm::BackendReply& reply(rdm::BackendReply::Kind kind, uint32_t id, rdm::MbxCode code);
};
