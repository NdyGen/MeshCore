#include "RdmSimMailbox.h"

#include <MailboxMesh.h>

#include <stdexcept>

namespace sim {

RdmSimMailbox::RdmSimMailbox(Simulator& sim, std::string name, int index)
    : SimNode(sim, std::move(name), index), _backend(_own) {
  pool_size = 32;
  _own.setTimeSource([this]() { return this->sim().wall_epoch(); });
  _on_boot = [this]() { _own.hello(); };
  _busy = [this]() { return _own.pendingReplies() > 0; };
}

RdmSimMailbox::RdmSimMailbox(Simulator& sim, std::string name, int index, rdm::MailboxBackend& backend,
                             std::function<void()> on_boot, std::function<bool()> busy)
    : SimNode(sim, std::move(name), index), _backend(backend), _on_boot(std::move(on_boot)), _busy(std::move(busy)) {
  pool_size = 32;
}

MemMailboxBackend& RdmSimMailbox::backend() {
  if (!ownsBackend()) throw std::logic_error("RdmSimMailbox runs on an external backend");
  return _own;
}

RdmSimMailbox::~RdmSimMailbox() = default;

MailboxMesh& RdmSimMailbox::mailbox() { return *_mbx; }

mesh::Mesh* RdmSimMailbox::createMesh() {
  _mbx = new MailboxMesh(radio(), millisClock(), rng(), rtc(), packetManager(), tables(), _backend, name().c_str());
  return _mbx;
}

void RdmSimMailbox::beginMesh() { _mbx->begin(); }

void RdmSimMailbox::loopMesh() { _mbx->loop(); }

void RdmSimMailbox::onBoot() {
  if (_on_boot) _on_boot();
}

bool RdmSimMailbox::busy() const { return SimNode::busy() || (_busy && _busy()); }

uint64_t RdmSimMailbox::nextWakeupMs() const {
  if (!powered() || !_mbx) return UINT64_MAX;
  uint32_t w = _mbx->nextWakeupMillis();
  return w == UINT32_MAX ? UINT64_MAX : const_cast<RdmSimMailbox*>(this)->sim().now() + w;
}

void RdmSimMailbox::advert(bool flood) {
  if (!powered()) return;
  Simulator::OnNode ctx(sim(), *this);
  _mbx->sendSelfAdvert(flood);
}

}
