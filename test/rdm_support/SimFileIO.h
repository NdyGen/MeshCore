#pragma once

#include <helpers/rdm/RdmStorage.h>
#include <SimFS.h>

#include <string.h>

// rdm::FileIO on a simulated node's flash (test/sim/SimFS.h). Honours SimFS::capacity_bytes and the torn-write
// budget SimFS::write_budget, so power loss in the simulator also cuts RDM writes short.
class SimFileIO : public rdm::FileIO {
  sim::SimFS& _fs;

  bool takeBudget(uint32_t len, uint32_t& allowed) {
    allowed = len;
    if (_fs.write_budget < 0) return true;
    if ((long)allowed > _fs.write_budget) allowed = (uint32_t)_fs.write_budget;
    _fs.write_budget -= allowed;
    return allowed == len;
  }

public:
  explicit SimFileIO(sim::SimFS& fs) : _fs(fs) {}

  bool exists(const char* path) override { return _fs.exists(path); }

  int32_t size(const char* path) override {
    auto it = _fs.files.find(path);
    return it == _fs.files.end() ? -1 : (int32_t)it->second.size();
  }

  bool create(const char* path, uint32_t size) override {
    if (_fs.write_budget == 0) return false;
    auto it = _fs.files.find(path);
    size_t replaced = it == _fs.files.end() ? 0 : it->second.size();
    if (_fs.used_bytes() - replaced + size > _fs.capacity_bytes) return false;
    _fs.files[path] = std::vector<uint8_t>(size, 0);
    return true;
  }

  bool read(const char* path, uint32_t off, uint8_t* buf, uint32_t len) override {
    auto it = _fs.files.find(path);
    if (it == _fs.files.end() || (uint64_t)off + len > it->second.size()) return false;
    memcpy(buf, it->second.data() + off, len);
    return true;
  }

  bool write(const char* path, uint32_t off, const uint8_t* buf, uint32_t len) override {
    auto it = _fs.files.find(path);
    if (it == _fs.files.end() || (uint64_t)off + len > it->second.size()) return false;
    uint32_t allowed;
    bool complete = takeBudget(len, allowed);
    memcpy(it->second.data() + off, buf, allowed);
    return complete;
  }

  bool remove(const char* path) override { return _fs.files.erase(path) != 0; }

  uint32_t freeBytes() override {
    size_t used = _fs.used_bytes();
    return used >= _fs.capacity_bytes ? 0 : (uint32_t)(_fs.capacity_bytes - used);
  }
};
