#pragma once

#include "Sim.h"

#include <helpers/BaseChatMesh.h>

#include <string>
#include <vector>

namespace sim {

// What a phone app does on top of the firmware: resend with attempt+1 after each timeout, and fall back to
// flood (CMD_RESET_PATH) before the last attempt. The real app logic lives outside this repo.
struct AppRetry {
  int max_attempts = 3;
  bool flood_on_last = true;
};

// Chat client on the real BaseChatMesh. It deliberately mirrors companion firmware defaults (timeouts, flood
// scope unscoped, contacts persisted in flash) but keeps an unbounded sent/inbox log so tests can assert on it.
// Anything the phone app would do (retries, path reset) is explicit in the test API, never implicit.
class ChatNode : public SimNode {
public:
  enum class Status { PENDING, DELIVERED, TIMED_OUT, SEND_FAILED };

  struct SentMsg {
    int id;
    int to_index;
    std::string text;
    uint32_t timestamp;
    uint8_t attempt;
    uint32_t expected_ack;
    uint32_t est_timeout_ms;
    bool flood;
    Status status;
    bool timed_out_before_ack;
    uint64_t sent_at_ms;
    uint64_t acked_at_ms;
  };

  struct RxMsg {
    int from_index;  // -1 if the sender is not a simulated node
    std::string text;
    uint32_t sender_timestamp;
    uint8_t txt_type;
    bool flood;
    uint8_t path_len;
    uint64_t at_ms;
  };

  ChatNode(Simulator& sim, std::string name, int index);
  ~ChatNode() override;

  // Advertise self (flood by default, like the app's "Advert" button).
  void advert(bool flood = true);
  // Put `other` in our contacts without an advert exchange (no path known yet).
  void add_contact(const SimNode& other);
  bool knows(const SimNode& other);
  bool has_path_to(const SimNode& other);
  uint8_t path_len_to(const SimNode& other);
  void reset_path(const SimNode& other);

  // Sends one TXT_MSG (TXT_TYPE_PLAIN) like CMD_SEND_TXT_MSG. Returns message id, or -1 if the contact is unknown.
  int send_text(const SimNode& to, const std::string& text, uint8_t attempt = 0, uint32_t timestamp = 0);
  int send_text_with_retries(const SimNode& to, const std::string& text, AppRetry policy = AppRetry());

  const SentMsg& sent(int id) const { return _sent.at(id); }
  const std::vector<SentMsg>& sent_log() const { return _sent; }
  const std::vector<RxMsg>& inbox() const { return _inbox; }
  size_t inbox_count(const std::string& text) const;
  // Final status of a logical message sent with send_text_with_retries (DELIVERED if any attempt was acked).
  Status retry_status(int retry_id) const;
  bool retry_done(int retry_id) const;

  BaseChatMesh& chat();

protected:
  mesh::Mesh* createMesh() override;
  void onBoot() override;
  void loopMesh() override;
  void onLoop() override;
  uint64_t nextWakeupMs() const override;

private:
  class Impl;
  friend class Impl;
  Impl* _impl = nullptr;

  struct RetryJob {
    int to_index;
    std::string text;
    uint32_t timestamp;
    AppRetry policy;
    std::vector<int> attempts;
    bool done;
  };

  std::vector<SentMsg> _sent;
  std::vector<RxMsg> _inbox;
  std::vector<RetryJob> _retries;

  ContactInfo* contactFor(const SimNode& other);
  int indexForPubKey(const uint8_t* pub_key) const;
  void saveContacts();
  void loadContacts();
  void markAcked(uint32_t ack);
};

// Plain repeater: real Mesh routing with forwarding enabled; no CLI, no ACL, default retransmit delays.
class RepeaterNode : public SimNode {
public:
  RepeaterNode(Simulator& sim, std::string name, int index) : SimNode(sim, std::move(name), index) { pool_size = 32; }
  void advert();

protected:
  mesh::Mesh* createMesh() override;
};

}
