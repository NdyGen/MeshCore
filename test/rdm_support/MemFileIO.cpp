#include "MemFileIO.h"

#include <string.h>

bool MemFileIO::exists(const char* path) {
  return files.count(path) != 0;
}

int32_t MemFileIO::size(const char* path) {
  auto it = files.find(path);
  return it == files.end() ? -1 : (int32_t)it->second.size();
}

uint32_t MemFileIO::usedBytes() const {
  uint64_t n = 0;
  for (const auto& f : files) n += f.second.size();
  return (uint32_t)n;
}

uint32_t MemFileIO::freeBytes() {
  uint32_t used = usedBytes();
  return used >= _capacity ? 0 : _capacity - used;
}

bool MemFileIO::create(const char* path, uint32_t size) {
  if (_fail_next_create) {
    _fail_next_create = false;
    return false;
  }
  if (_write_budget == 0) return false;
  auto it = files.find(path);
  uint32_t replaced = it == files.end() ? 0 : (uint32_t)it->second.size();
  if ((uint64_t)usedBytes() - replaced + size > _capacity) return false;
  files[path] = std::vector<uint8_t>(size, 0);
  return true;
}

bool MemFileIO::read(const char* path, uint32_t off, uint8_t* buf, uint32_t len) {
  auto it = files.find(path);
  if (it == files.end() || (uint64_t)off + len > it->second.size()) return false;
  memcpy(buf, it->second.data() + off, len);
  return true;
}

bool MemFileIO::write(const char* path, uint32_t off, const uint8_t* buf, uint32_t len) {
  auto it = files.find(path);
  if (it == files.end() || (uint64_t)off + len > it->second.size()) return false;
  _write_calls++;
  uint32_t n = len;
  if (_write_budget >= 0 && (long)n > _write_budget) n = (uint32_t)_write_budget;
  memcpy(it->second.data() + off, buf, n);
  _bytes_written += n;
  if (_write_budget >= 0) _write_budget -= n;
  return n == len;
}

bool MemFileIO::remove(const char* path) {
  return files.erase(path) != 0;
}

bool MemFileIO::corrupt(const char* path, uint32_t off, uint8_t xor_mask) {
  auto it = files.find(path);
  if (it == files.end() || off >= it->second.size()) return false;
  it->second[off] ^= xor_mask;
  return true;
}
