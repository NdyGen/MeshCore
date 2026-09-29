#include <Arduino.h>

#include "Sim.h"

namespace sim { Simulator* activeSimulator(); }

SimSerialPort Serial;

static bool verbose() {
  static int v = -1;
  if (v < 0) {
    const char* e = getenv("SIM_VERBOSE");
    v = (e && *e && *e != '0') ? 1 : 0;
  }
  return v == 1;
}

size_t SimSerialPort::write(uint8_t b) {
  if (verbose()) fputc(b, stdout);
  return 1;
}

size_t SimSerialPort::write(const uint8_t* buf, size_t len) {
  if (verbose()) fwrite(buf, 1, len, stdout);
  return len;
}

unsigned long millis() {
  sim::Simulator* s = sim::activeSimulator();
  if (!s) return 0;
  if (s->current()) return (unsigned long)s->current()->uptimeMs();
  return (unsigned long)s->now();
}

unsigned long micros() { return millis() * 1000UL; }

void delay(unsigned long) {}  // blocking waits would stall the whole simulated mesh

void yield() {}

static sim::SimRNG& activeRng() {
  static sim::SimRNG fallback;
  sim::Simulator* s = sim::activeSimulator();
  if (!s) return fallback;
  if (s->current()) return s->current()->rng();
  return s->rng();
}

long random(long max) { return max > 0 ? (long)(activeRng().next() % (uint64_t)max) : 0; }

long random(long min, long max) { return max > min ? min + random(max - min) : min; }

void randomSeed(unsigned long) {}

char* ultoa(unsigned long value, char* buf, int base) {
  char tmp[65];
  int i = 0;
  if (base < 2 || base > 36) base = 10;
  do {
    int d = (int)(value % base);
    tmp[i++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
    value /= base;
  } while (value);
  for (int j = 0; j < i; j++) buf[j] = tmp[i - 1 - j];
  buf[i] = 0;
  return buf;
}

char* ltoa(long value, char* buf, int base) {
  if (value < 0 && base == 10) {
    buf[0] = '-';
    ultoa((unsigned long)(-(value + 1)) + 1, buf + 1, base);
    return buf;
  }
  return ultoa((unsigned long)value, buf, base);
}
