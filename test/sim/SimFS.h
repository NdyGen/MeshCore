#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace sim {

// A node's flash. Survives power cycles and reboots; cleared only by wipe(). Writes are durable as soon as
// File::write() returns (no page cache). Optional capacity limit and a byte budget to cut writes short
// (torn write at power loss).
struct SimFS {
  std::map<std::string, std::vector<uint8_t>> files;
  size_t capacity_bytes = 1024 * 1024;
  long write_budget = -1;  // -1 = unlimited; otherwise bytes that may still be written before writes start failing
  bool power_off_on_exhaust = false;
  // Called for every write that reaches this flash (fs::File, and FileIO adapters that report it), for the
  // simulator's fs_log. complete = false: the write was cut short by write_budget.
  std::function<void(const std::string& path, uint32_t off, uint32_t len, bool complete)> on_write;

  bool exists(const std::string& path) const { return files.count(path) != 0; }
  void wipe() { files.clear(); }
  size_t used_bytes() const {
    size_t n = 0;
    for (const auto& f : files) n += f.second.size();
    return n;
  }
  enum class OnExhaust { FAIL_WRITES, POWER_OFF };
  // Only `bytes` more bytes reach flash. FAIL_WRITES: later writes fail and the firmware sees the error.
  // POWER_OFF: the node loses power right after the loop in which the budget ran out (a crash mid-write).
  void fail_writes_after(long bytes, OnExhaust mode = OnExhaust::FAIL_WRITES) {
    write_budget = bytes;
    power_off_on_exhaust = mode == OnExhaust::POWER_OFF;
  }
  void report_write(const std::string& path, uint32_t off, uint32_t len, bool complete) {
    if (on_write) on_write(path, off, len, complete);
  }
};

}
