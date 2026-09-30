#pragma once

#include "Sim.h"
#include "SimChatMesh.h"

#include <functional>
#include <string>
#include <vector>

namespace sim {

// What a phone app does on top of the firmware: resend with attempt+1 after each timeout, and fall back to
// flood (CMD_RESET_PATH) before the last attempt. The real app logic lives outside this repo.
struct AppRetry {
  int max_attempts = 3;
  bool flood_on_last = true;
};

enum class MsgStatus { PENDING, DELIVERED, TIMED_OUT, SEND_FAILED };

// The app's retry policy applied to a log of attempts. The owner keeps the attempts (with their status) and does
// the sending; the tracker decides when the next attempt goes out and what a logical message's final status is.
class RetryTracker {
public:
  using StatusFn = std::function<MsgStatus(int attempt_id)>;
  using ResetPathFn = std::function<void(int to_index)>;
  // Sends the next attempt; -1 when it cannot (the contact is unknown, the app is not connected), which ends the job.
  using SendFn = std::function<int(int to_index, const std::string& text, uint8_t attempt, uint32_t timestamp)>;

  int add(int to_index, const std::string& text, uint32_t timestamp, AppRetry policy, int first_attempt_id) {
    _jobs.push_back(Job{to_index, text, timestamp, policy, {first_attempt_id}, false});
    return (int)_jobs.size() - 1;
  }

  // DELIVERED ends a job; a timed-out or failed last attempt gets the next one, after a path reset before the
  // last attempt when the policy floods there.
  void tick(const StatusFn& status, const ResetPathFn& reset_path, const SendFn& send) {
    for (auto& job : _jobs) {
      if (job.done) continue;
      MsgStatus last = status(job.attempts.back());
      if (last == MsgStatus::DELIVERED) {
        job.done = true;
      } else if (last == MsgStatus::TIMED_OUT || last == MsgStatus::SEND_FAILED) {
        int n = (int)job.attempts.size();
        if (n >= job.policy.max_attempts) {
          job.done = true;
          continue;
        }
        if (job.policy.flood_on_last && n == job.policy.max_attempts - 1) reset_path(job.to_index);
        int id = send(job.to_index, job.text, (uint8_t)n, job.timestamp);
        if (id < 0) job.done = true; else job.attempts.push_back(id);
      }
    }
  }

  // Final status of a logical message (DELIVERED if any attempt was acked).
  MsgStatus status(int retry_id, const StatusFn& status_of) const {
    const Job& job = _jobs.at(retry_id);
    for (int id : job.attempts) {
      if (status_of(id) == MsgStatus::DELIVERED) return MsgStatus::DELIVERED;
    }
    return status_of(job.attempts.back());
  }
  bool done(int retry_id) const { return _jobs.at(retry_id).done; }

private:
  struct Job {
    int to_index;
    std::string text;
    uint32_t timestamp;
    AppRetry policy;
    std::vector<int> attempts;
    bool done;
  };
  std::vector<Job> _jobs;
};

// Messages in an inbox log with exactly this text.
template <class Msg>
size_t countText(const std::vector<Msg>& inbox, const std::string& text) {
  size_t n = 0;
  for (const auto& m : inbox) n += m.text == text;
  return n;
}

// Chat client on the real BaseChatMesh. It deliberately mirrors companion firmware defaults (timeouts, flood
// scope unscoped, contacts persisted in flash) but keeps an unbounded sent/inbox log so tests can assert on it.
// Anything the phone app would do (retries, path reset) is explicit in the test API, never implicit.
class ChatNode : public SimNode {
public:
  using Status = MsgStatus;

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
  const std::vector<RxMsg>& inbox() const { return _inbox; }
  size_t inbox_count(const std::string& text) const { return countText(_inbox, text); }
  // Final status of a logical message sent with send_text_with_retries (DELIVERED if any attempt was acked).
  Status retry_status(int retry_id) const;
  bool retry_done(int retry_id) const { return _retries.done(retry_id); }

  BaseChatMesh& chat();

protected:
  MeshPtr createMesh() override;
  void onBoot() override;
  void loopMesh() override;
  void onLoop() override;
  uint64_t nextWakeupMs() const override;

private:
  class Impl;
  friend class Impl;
  Impl* _impl = nullptr;

  std::vector<SentMsg> _sent;
  std::vector<RxMsg> _inbox;
  RetryTracker _retries;

  ContactInfo* contactFor(const SimNode& other);
  void saveContacts();
  void markAcked(uint32_t ack);
};

// Plain repeater: real Mesh routing with forwarding enabled; no CLI, no ACL, default retransmit delays.
class RepeaterNode : public SimNode {
public:
  RepeaterNode(Simulator& sim, std::string name, int index) : SimNode(sim, std::move(name), index) { pool_size = 32; }
  void advert();

protected:
  MeshPtr createMesh() override;
};

}
