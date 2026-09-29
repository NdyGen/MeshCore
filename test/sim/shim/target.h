#pragma once

// Simulated "variant" for examples/companion_radio: the globals a real target.h provides. Radio I/O does not go
// through radio_driver (MyMesh receives the per-node SimRadio in its constructor); these stubs only cover the
// board/driver calls MyMesh makes for stats, prefs and reboot.

#include <Mesh.h>
#include <helpers/KeyValueStore.h>
#include <helpers/SensorManager.h>

class SimBoard : public mesh::MainBoard {
public:
  uint16_t getBattMilliVolts() override { return 4100; }
  float getMCUTemperature() override { return 25.0f; }
  const char* getManufacturerName() const override { return "MeshCore Simulator"; }
  uint8_t getStartupReason() const override { return BD_STARTUP_NORMAL; }
  void reboot() override;    // deferred: the simulator reboots the current node after its loop() returns
  void powerOff() override;  // deferred power-off of the current node
  void attachDynamicPrefs(KeyValueStore*) {}
};

class SimRadioDriver {
public:
  void setParams(float freq, float bw, uint8_t sf, uint8_t cr) {}
  void setTxPower(int8_t dbm) {}
  bool setRxBoostedGainMode(bool) { return false; }
  bool getRxBoostedGainMode() const { return false; }
  float getLastRSSI() const { return 0; }
  float getLastSNR() const { return 0; }
  uint32_t getPacketsRecv() const { return 0; }
  uint32_t getPacketsSent() const { return 0; }
  uint32_t getPacketsRecvErrors() const { return 0; }
};

extern SimBoard board;
extern SimRadioDriver radio_driver;
extern SensorManager sensors;

mesh::LocalIdentity radio_new_identity();
