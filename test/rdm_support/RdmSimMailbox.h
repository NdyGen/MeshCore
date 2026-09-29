#pragma once

#include <Sim.h>

#include "MemMailboxBackend.h"

#include <functional>

class MailboxMesh;

namespace sim {

// Mailbox M in the simulator: the real examples/mailbox_server MailboxMesh on a sim node. The backend plays the Pi:
// it lives outside the node's RAM, so it survives power_off/reboot. By default the node owns a MemMailboxBackend on
// the sim wall clock and says HELLO to it at every boot. An external backend (e.g. real mbxd behind a
// SubprocessBackend) is not owned; on_boot then does what HELLO needs, and busy tells fast-forward that replies
// are still on their way.
class RdmSimMailbox : public SimNode {
public:
  RdmSimMailbox(Simulator& sim, std::string name, int index);
  RdmSimMailbox(Simulator& sim, std::string name, int index, rdm::MailboxBackend& backend,
                std::function<void()> on_boot = nullptr, std::function<bool()> busy = nullptr);
  ~RdmSimMailbox() override;

  bool ownsBackend() const { return &_backend == &_own; }
  MemMailboxBackend& backend();   // the owned one; std::logic_error with an external backend
  MailboxMesh& mailbox();
  void advert(bool flood = true);

protected:
  mesh::Mesh* createMesh() override;
  void beginMesh() override;
  void loopMesh() override;
  void onBoot() override;
  bool busy() const override;
  uint64_t nextWakeupMs() const override;

private:
  MemMailboxBackend _own;
  rdm::MailboxBackend& _backend;
  std::function<void()> _on_boot;
  std::function<bool()> _busy;
  MailboxMesh* _mbx = nullptr;
};

}
