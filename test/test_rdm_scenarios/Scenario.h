#pragma once

// Scenario world for 06 par. 6.2: Bob and Alice as the real companion firmware (MyMesh in CompanionNode) with an
// RdmSimApp each over companion frames, repeater R between them, mailbox M (MemMailboxBackend, or the real daemon
// through SubprocessBackend) at R. Everything a scenario needs beyond the simulator: setup up to the scenario's start
// state, running with the apps ticking, and decoded wires.

#include <gtest/gtest.h>

#include <ChatNode.h>
#include <CompanionNode.h>
#include <RdmScenario.h>
#include <RdmSimApp.h>
#include <RdmSimMailbox.h>
#include <SubprocessBackend.h>
#include <helpers/rdm/RdmContacts.h>

#include <stdlib.h>
#include <string.h>

#include <memory>
#include <string>
#include <vector>

namespace sim {

enum class Layout : uint8_t {
  VIA_REPEATER,   // B-R-A, mailboxes at R: the behaviour topology of 06 par. 6.2
  DIRECT          // every pair linked (minus B-A when forbidden): the airtime topology, one hop per packet
};

struct WorldOptions {
  Layout   layout = Layout::VIA_REPEATER;
  int      mailboxes = 0;          // 0, 1 or 2
  bool     real_mbxd = false;      // mailbox 0 runs the daemon through SubprocessBackend
  bool     ab_link = true;         // DIRECT: link B-A (06 S07: no direct link B-A)
  bool     warmup = true;          // one delivered DM first: paths, cap and (with a mailbox) MBX_INFO known
  bool     bob_rdm_client = true;  // false: Bob's app is the standard app (no CMD_RDM_ENABLE)
  uint32_t alice_rdm_flash = 0;    // > 0: Alice's flash has only this much free for RDM (fewer register slots)
  uint64_t seed = 42;
};

class World {
public:
  Simulator s;
  CompanionNode* bob = nullptr;
  CompanionNode* alice = nullptr;
  RepeaterNode* rep = nullptr;
  std::vector<SimNode*> mbx;
  std::unique_ptr<RdmSimApp> bob_app, alice_app;
  WireLog wires{s};
  uint8_t k_owner[2][16];
  std::unique_ptr<SubprocessBackend> mbxd;
  std::string mbxd_dir;

  explicit World(const WorldOptions& o);
  ~World();

  RdmSimApp& bobApp() { return *bob_app; }
  RdmSimApp& aliceApp() { return *alice_app; }
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

  bool bobState(int bob_msg, rdm::OutState st) { return outState(*bob, *alice, bob_app->sent(bob_msg).ts) == (int)st; }
  // Runtime switch to upstream behaviour (scenario 10); lasts until the next boot.
  void setAliceRdm(bool on) {
    Simulator::OnNode ctx(s, *alice);
    alice->firmware().rdm().setEnabled(on);
  }

  // Bob's 0x91 states and Alice's display for one message, for diagnostics.
  std::string trace(int bob_msg);

  WorldOptions opt;

private:
  void introduce();
};

inline std::vector<rdm::UserStatus> S(std::initializer_list<rdm::UserStatus> l) { return l; }

}
