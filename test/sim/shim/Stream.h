#pragma once

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#define DEC 10
#define HEX 16
#define OCT 8
#define BIN 2

class Print {
public:
  virtual ~Print() = default;
  virtual size_t write(uint8_t b) = 0;
  virtual size_t write(const uint8_t* buf, size_t len) {
    size_t n = 0;
    for (size_t i = 0; i < len; i++) n += write(buf[i]);
    return n;
  }
  size_t write(const char* str) { return str ? write((const uint8_t*)str, strlen(str)) : 0; }
  size_t write(const char* buf, size_t len) { return write((const uint8_t*)buf, len); }

  size_t printf(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) return 0;
    return write((const uint8_t*)buf, (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1);
  }
  size_t print(const char* s) { return write(s); }
  size_t print(char c) { return write((uint8_t)c); }
  size_t print(int v, int base = DEC) { return printNum((long)v, base); }
  size_t print(unsigned int v, int base = DEC) { return printNum((long)v, base); }
  size_t print(long v, int base = DEC) { return printNum(v, base); }
  size_t print(unsigned long v, int base = DEC) { return printNum((long)v, base); }
  size_t print(double v, int digits = 2) { return printf("%.*f", digits, v); }
  size_t println() { return write("\n"); }
  template <typename T> size_t println(T v) { size_t n = print(v); return n + println(); }
  template <typename T> size_t println(T v, int fmt) { size_t n = print(v, fmt); return n + println(); }
  virtual void flush() {}

private:
  size_t printNum(long v, int base) {
    if (base == HEX) return printf("%lX", v);
    if (base == OCT) return printf("%lo", v);
    return printf("%ld", v);
  }
};

class Stream : public Print {
public:
  virtual int available() { return 0; }
  virtual int read() { return -1; }
  virtual int peek() { return -1; }
  virtual size_t readBytes(uint8_t* buf, size_t len) {
    size_t n = 0;
    while (n < len) {
      int c = read();
      if (c < 0) break;
      buf[n++] = (uint8_t)c;
    }
    return n;
  }
  size_t readBytes(char* buf, size_t len) { return readBytes((uint8_t*)buf, len); }
  size_t write(uint8_t) override { return 1; }
  using Print::write;
};
