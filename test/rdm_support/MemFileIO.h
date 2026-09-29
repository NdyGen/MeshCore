#pragma once

#include <helpers/rdm/RdmStorage.h>

#include <map>
#include <string>
#include <vector>

// In-memory rdm::FileIO for unit tests, with fault injection for torn writes, lost files and full flash.
class MemFileIO : public rdm::FileIO {
public:
  std::map<std::string, std::vector<uint8_t>> files;

  explicit MemFileIO(uint32_t capacity = 1024 * 1024) : _capacity(capacity) {}

  bool     exists(const char* path) override;
  int32_t  size(const char* path) override;
  bool     create(const char* path, uint32_t size) override;
  bool     read(const char* path, uint32_t off, uint8_t* buf, uint32_t len) override;
  bool     write(const char* path, uint32_t off, const uint8_t* buf, uint32_t len) override;
  bool     remove(const char* path) override;
  uint32_t freeBytes() override;

  // Power dies mid-write: only `bytes` more bytes reach flash, every later write or create fails.
  void failWritesAfter(long bytes) { _write_budget = bytes; }
  void failNextCreate() { _fail_next_create = true; }
  void clearFaults() { _write_budget = -1; _fail_next_create = false; }
  void setCapacity(uint32_t bytes) { _capacity = bytes; }
  void wipeAll() { files.clear(); }
  bool corrupt(const char* path, uint32_t off, uint8_t xor_mask);
  uint32_t usedBytes() const;
  uint32_t writeCalls() const { return _write_calls; }
  uint64_t bytesWritten() const { return _bytes_written; }

private:
  uint32_t _capacity;
  long     _write_budget = -1;
  bool     _fail_next_create = false;
  uint32_t _write_calls = 0;
  uint64_t _bytes_written = 0;
};
