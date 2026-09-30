#pragma once

#include <stddef.h>
#include <stdint.h>

namespace rdm {

class FileIO {
public:
  virtual ~FileIO() {}
  virtual bool     exists(const char* path) = 0;
  virtual int32_t  size(const char* path) = 0;                       // -1 if the file is missing
  virtual bool     create(const char* path, uint32_t size) = 0;      // new zero-filled file, replaces an existing one
  virtual bool     read(const char* path, uint32_t off, uint8_t* buf, uint32_t len) = 0;
  virtual bool     write(const char* path, uint32_t off, const uint8_t* buf, uint32_t len) = 0;  // in place
  virtual bool     remove(const char* path) = 0;
  virtual uint32_t freeBytes() = 0;
};

// File format: header "RDMF" | ver(1) | ab(1) | payload_size(2) | slots(2) | reserved(6) = 16 bytes,
// then per copy: seq(4) | crc16(2) | used(1) | pad(1) | payload. CRC-16/CCITT over seq|used|payload.
// ab = true: two copies per slot; read takes the valid one with the highest seq, write overwrites the other.
class RecordFile {
public:
  enum class Open : uint8_t { OPENED, CREATED, MIGRATED, FAILED };
  typedef bool (*MigrateFn)(uint8_t from_ver, const uint8_t* in, uint16_t in_size, uint8_t* out);

  RecordFile(FileIO& io, const char* path, uint8_t format_ver, uint16_t payload_size, uint16_t slots, bool ab);
  Open     open(MigrateFn migrate = nullptr);   // other slots/ab: MIGRATED (records kept, excess slots dropped);
                                                // other payload_size/version without migrate: CREATED. Paths <= 35 chars.
  bool     read(uint16_t slot, uint8_t* payload) const;   // false: empty or CRC error
  bool     write(uint16_t slot, const uint8_t* payload);
  bool     erase(uint16_t slot);
  uint16_t slots() const;
  uint16_t payloadSize() const;
  static uint32_t fileSize(uint16_t payload_size, uint16_t slots, bool ab);

private:
  FileIO&     _io;
  const char* _path;
  uint8_t     _ver;
  uint16_t    _payload_size;
  uint16_t    _slots;
  bool        _ab;
  bool        _open = false;

  struct Header { uint8_t ver; bool ab; uint16_t payload_size; uint16_t slots; };

  uint8_t  copies() const { return _ab ? 2 : 1; }
  uint32_t copyOffset(uint16_t slot, uint8_t copy) const;
  void     readCopyHeaders(uint16_t slot, uint32_t seq[2], uint16_t crc[2], bool used[2]) const;   // seq 0: never written
  bool     payloadCrc(uint16_t slot, uint8_t copy, uint16_t& crc) const;
  bool     newestCopy(uint16_t slot, const uint32_t seq[2], const uint16_t crc[2], const bool used[2], uint8_t& copy) const;
  bool     writeCopy(uint16_t slot, const uint8_t* payload);   // nullptr: erase
  bool     writeHeader(const char* path) const;
  bool     readHeader(const char* path, Header& h) const;
  bool     matchesLayout(const char* path) const;
  Open     createFresh();
  Open     migrate(const char* tmp, const Header& old, MigrateFn fn);
  bool     finishMigration(const char* tmp);
};

}
