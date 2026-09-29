#pragma once

// Arduino-core style fs::FS / fs::File (ESP32/RP2040 API shape) backed by a node's sim::SimFS.

#include <Stream.h>

#include <memory>
#include <string>

#include "SimFS.h"

namespace fs {

enum SeekMode { SeekSet = 0, SeekCur = 1, SeekEnd = 2 };

struct FSInfo {
  size_t totalBytes = 0;
  size_t usedBytes = 0;
  size_t blockSize = 4096;
  size_t pageSize = 256;
  size_t maxOpenFiles = 16;
  size_t maxPathLength = 64;
};

class File : public Stream {
  struct Handle {
    sim::SimFS* store;
    std::string path;
    size_t pos;
    bool readable, writable, append, is_dir;
    std::vector<std::string> dir_entries;
    size_t dir_next;
  };
  std::shared_ptr<Handle> _h;

  std::vector<uint8_t>* data() const;

public:
  File() = default;
  static File openFile(sim::SimFS& store, const std::string& path, bool readable, bool writable, bool append);
  static File openDir(sim::SimFS& store, const std::string& path);

  explicit operator bool() const { return _h != nullptr; }

  size_t write(uint8_t b) override { return write(&b, 1); }
  size_t write(const uint8_t* buf, size_t len) override;
  using Print::write;

  int available() override;
  int read() override;
  int peek() override;
  size_t read(uint8_t* buf, size_t len);
  size_t readBytes(uint8_t* buf, size_t len) override { return read(buf, len); }
  using Stream::readBytes;

  bool seek(uint32_t pos, SeekMode mode = SeekSet);
  size_t position() const { return _h ? _h->pos : 0; }
  size_t size() const;
  void flush() override {}
  void close() { _h.reset(); }
  const char* name() const;
  const char* path() const { return _h ? _h->path.c_str() : ""; }
  bool isDirectory() const { return _h && _h->is_dir; }
  File openNextFile(const char* mode = "r");
  void rewindDirectory() { if (_h) _h->dir_next = 0; }
};

class FS {
public:
  virtual ~FS() = default;
  virtual sim::SimFS& store() = 0;

  bool begin(bool formatOnFail = false) { return true; }
  void end() {}
  File open(const char* path, const char* mode = "r", bool create = false);
  File open(const std::string& path, const char* mode = "r", bool create = false) { return open(path.c_str(), mode, create); }
  bool exists(const char* path);
  bool remove(const char* path);
  bool rename(const char* from, const char* to);
  bool mkdir(const char* path) { return true; }  // flat store: directories are implicit
  bool rmdir(const char* path) { return true; }
  bool format() { store().wipe(); return true; }
  bool info(FSInfo& info);
  size_t totalBytes() { return store().capacity_bytes; }
  size_t usedBytes() { return store().used_bytes(); }
};

// A fixed node's flash (DataStore gets one of these per boot).
class BoundFS : public FS {
  sim::SimFS& _store;
public:
  explicit BoundFS(sim::SimFS& s) : _store(s) {}
  sim::SimFS& store() override { return _store; }
};

// The flash of whichever node is currently running (for firmware code that uses a global like LittleFS).
class CurrentNodeFS : public FS {
public:
  sim::SimFS& store() override;
};

}

using fs::File;
using fs::FS;
using fs::FSInfo;
using fs::SeekMode;
using fs::SeekSet;
using fs::SeekCur;
using fs::SeekEnd;
