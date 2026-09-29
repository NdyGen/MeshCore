#pragma once

#include <stddef.h>
#include <stdint.h>

// Little-endian field access for records and wire bodies. Internal to RDM: the core must not pull in
// mesh::Utils (Utils.h includes MeshCore.h, which brings in Arduino.h and Stream.h).

namespace rdm {

inline void put16(uint8_t* p, uint16_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
}

inline uint16_t get16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }

inline void put32(uint8_t* p, uint32_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

inline uint32_t get32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

inline bool isZero(const uint8_t* p, size_t n) {
  for (size_t i = 0; i < n; i++)
    if (p[i]) return false;
  return true;
}

}
