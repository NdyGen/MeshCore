#pragma once

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

// Deterministic test data shared by the RDM suites. The parameters reproduce the ad-hoc generators these replace
// byte for byte, so existing vectors and expected values stay valid.

// p[i] = seed + i * mul + add (mod 256). The old per-test variants map onto it: `seed * 13 + i` is
// fillPattern(p, n, seed * 13, 1), `seed * 31 + i * 7 + 1` is fillPattern(p, n, seed * 31, 7, 1).
inline void fillPattern(uint8_t* p, size_t n, uint32_t seed, uint32_t mul = 7, uint32_t add = 0) {
  for (size_t i = 0; i < n; i++) p[i] = (uint8_t)(seed + i * mul + add);
}

struct TestKey {
  uint8_t pub[32];
  explicit TestKey(uint32_t seed, uint32_t mul = 7, uint32_t add = 0) { fillPattern(pub, sizeof(pub), seed, mul, add); }
};

// Lower case, as mbxd writes it (mesh::Utils::toHex writes upper case).
inline std::string toHexLower(const uint8_t* p, size_t n) {
  static const char digits[] = "0123456789abcdef";
  std::string s;
  s.reserve(n * 2);
  for (size_t i = 0; i < n; i++) {
    s += digits[p[i] >> 4];
    s += digits[p[i] & 15];
  }
  return s;
}

inline std::string toHexLower(const std::vector<uint8_t>& v) { return toHexLower(v.data(), v.size()); }

inline int hexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// false on an odd length or a non-hex character; either case is accepted
inline bool fromHex(const std::string& s, std::vector<uint8_t>& out) {
  if (s.size() % 2) return false;
  std::vector<uint8_t> b(s.size() / 2);
  for (size_t i = 0; i < b.size(); i++) {
    int hi = hexNibble(s[2 * i]), lo = hexNibble(s[2 * i + 1]);
    if (hi < 0 || lo < 0) return false;
    b[i] = (uint8_t)(hi << 4 | lo);
  }
  out.swap(b);
  return true;
}

// exactly n bytes
inline bool fromHex(const std::string& s, uint8_t* out, size_t n) {
  std::vector<uint8_t> b;
  if (s.size() != n * 2 || !fromHex(s, b)) return false;
  for (size_t i = 0; i < n; i++) out[i] = b[i];
  return true;
}

// Seeded linear congruential generator. Defaults are the Numerical Recipes constants of the outbox and mailbox
// doubles; the node wire rig uses {seed, 1103515245u, 12345u}. Callers keep their own output shift.
struct Lcg32 {
  uint32_t s = 0;
  uint32_t mul = 1664525u;
  uint32_t add = 1013904223u;

  uint32_t next() {
    s = s * mul + add;
    return s;
  }
};
