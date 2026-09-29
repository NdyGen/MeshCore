#pragma once

// Subset of electroniccats/CayenneLPP used by examples/companion_radio (telemetry responses).

#include <cstddef>
#include <cstdint>
#include <vector>

class CayenneLPP {
  std::vector<uint8_t> _buf;
  size_t _max;

  uint8_t add(uint8_t channel, uint8_t type, int32_t value, int bytes) {
    if (_buf.size() + 2 + bytes > _max) return 0;
    _buf.push_back(channel);
    _buf.push_back(type);
    for (int i = bytes - 1; i >= 0; i--) _buf.push_back((uint8_t)(value >> (8 * i)));
    return (uint8_t)_buf.size();
  }

public:
  explicit CayenneLPP(size_t size) : _max(size) {}
  void reset() { _buf.clear(); }
  uint8_t getSize() const { return (uint8_t)_buf.size(); }
  uint8_t* getBuffer() { return _buf.data(); }
  uint8_t addVoltage(uint8_t channel, float v) { return add(channel, 116, (int32_t)(v * 100), 2); }
  uint8_t addTemperature(uint8_t channel, float c) { return add(channel, 103, (int32_t)(c * 10), 2); }
};
