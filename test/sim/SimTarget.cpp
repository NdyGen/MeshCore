#include <LittleFS.h>
#include <target.h>

#include "Sim.h"

namespace sim { Simulator* activeSimulator(); }

SimBoard board;
SimRadioDriver radio_driver;
SensorManager sensors;
fs::CurrentNodeFS LittleFS;

static sim::SimNode* currentNode() {
  sim::Simulator* s = sim::activeSimulator();
  return s ? s->current() : nullptr;
}

void SimBoard::reboot() {
  if (auto n = currentNode()) n->request_reboot();
}

void SimBoard::powerOff() {
  if (auto n = currentNode()) n->request_power_off();
}

mesh::LocalIdentity radio_new_identity() {
  sim::SimNode* n = currentNode();
  sim::SimRNG fallback;
  return mesh::LocalIdentity(n ? static_cast<mesh::RNG*>(&n->rng()) : &fallback);
}
