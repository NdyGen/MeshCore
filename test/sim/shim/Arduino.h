#pragma once

// Minimal Arduino core for the native simulator. Time and randomness are routed through the
// simulator so that code which calls millis()/random() directly (e.g. companion MyMesh) stays
// deterministic and sees per-node uptime.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <type_traits>

#include "Stream.h"

using std::isnan;

#ifndef PROGMEM
  #define PROGMEM
#endif
#ifndef F
  #define F(s) (s)
#endif

unsigned long millis();
unsigned long micros();
void delay(unsigned long ms);
void yield();
long random(long max);
long random(long min, long max);
void randomSeed(unsigned long seed);

char* ltoa(long value, char* buf, int base);
char* ultoa(unsigned long value, char* buf, int base);
inline char* itoa(int value, char* buf, int base) { return ltoa(value, buf, base); }
inline char* utoa(unsigned int value, char* buf, int base) { return ultoa(value, buf, base); }

template <typename A, typename B>
inline typename std::common_type<A, B>::type min(A a, B b) { return b < a ? b : a; }
template <typename A, typename B>
inline typename std::common_type<A, B>::type max(A a, B b) { return a < b ? b : a; }
template <typename T, typename L, typename H>
inline T constrain(T x, L lo, H hi) { return x < lo ? lo : (x > hi ? hi : x); }

class SimSerialPort : public Stream {
public:
  size_t write(uint8_t b) override;
  size_t write(const uint8_t* buf, size_t len) override;
  using Print::write;
  void begin(unsigned long) {}
  explicit operator bool() const { return true; }
};

extern SimSerialPort Serial;
