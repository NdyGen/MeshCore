#include "RdmStorage.h"

#include <string.h>

namespace rdm {

static const uint32_t HEADER_SIZE = 16;
static const uint32_t COPY_HEADER_SIZE = 8;
static const uint8_t MAGIC[4] = {'R', 'D', 'M', 'F'};
static const uint16_t MAX_MIGRATE_PAYLOAD = 256;
static const char TMP_SUFFIX[] = ".mig";
static const uint32_t CHUNK = 32;

static uint16_t crc16(uint16_t crc, const uint8_t* d, uint32_t n) {   // CRC-16/CCITT, poly 0x1021
  for (uint32_t i = 0; i < n; i++) {
    crc ^= (uint16_t)d[i] << 8;
    for (int b = 0; b < 8; b++) crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
  }
  return crc;
}

static void put16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t* p, uint32_t v) { put16(p, (uint16_t)v); put16(p + 2, (uint16_t)(v >> 16)); }
static uint16_t get16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t get32(const uint8_t* p) { return get16(p) | ((uint32_t)get16(p + 2) << 16); }

static uint16_t crcStart(uint32_t seq, bool used) {
  uint8_t b[5];
  put32(b, seq);
  b[4] = used ? 1 : 0;
  return crc16(0xFFFF, b, 5);
}

RecordFile::RecordFile(FileIO& io, const char* path, uint8_t format_ver, uint16_t payload_size, uint16_t slots, bool ab)
  : _io(io), _path(path), _ver(format_ver), _payload_size(payload_size), _slots(slots), _ab(ab) {
}

uint16_t RecordFile::slots() const { return _slots; }
uint16_t RecordFile::payloadSize() const { return _payload_size; }

uint32_t RecordFile::fileSize(uint16_t payload_size, uint16_t slots, bool ab) {
  return HEADER_SIZE + (uint32_t)slots * (ab ? 2 : 1) * (COPY_HEADER_SIZE + payload_size);
}

uint32_t RecordFile::copyOffset(uint16_t slot, uint8_t copy) const {
  return HEADER_SIZE + ((uint32_t)slot * copies() + copy) * (COPY_HEADER_SIZE + _payload_size);
}

bool RecordFile::readCopyHeader(uint16_t slot, uint8_t copy, uint32_t& seq, uint16_t& crc, bool& used) const {
  uint8_t h[COPY_HEADER_SIZE];
  if (!_io.read(_path, copyOffset(slot, copy), h, sizeof(h))) return false;
  seq = get32(h);
  crc = get16(&h[4]);
  used = h[6] != 0;
  return seq != 0;   // seq 0 is a never-written copy
}

bool RecordFile::copyValid(uint16_t slot, uint8_t copy, uint32_t& seq, bool& used) const {
  uint16_t stored;
  if (!readCopyHeader(slot, copy, seq, stored, used)) return false;
  uint16_t crc = crcStart(seq, used);
  uint32_t off = copyOffset(slot, copy) + COPY_HEADER_SIZE;
  uint8_t buf[CHUNK];
  for (uint32_t done = 0; done < _payload_size;) {
    uint32_t n = _payload_size - done < CHUNK ? _payload_size - done : CHUNK;
    if (!_io.read(_path, off + done, buf, n)) return false;
    crc = crc16(crc, buf, n);
    done += n;
  }
  return crc == stored;
}

bool RecordFile::newestCopy(uint16_t slot, uint8_t& copy, uint32_t& seq, bool& used) const {
  bool found = false;
  for (uint8_t c = 0; c < copies(); c++) {
    uint32_t s;
    bool u;
    if (copyValid(slot, c, s, u) && (!found || s > seq)) {
      found = true;
      copy = c;
      seq = s;
      used = u;
    }
  }
  return found;
}

bool RecordFile::read(uint16_t slot, uint8_t* payload) const {
  if (!_open || slot >= _slots) return false;
  uint32_t seq[2] = {0, 0};
  uint16_t crc[2] = {0, 0};
  bool used[2] = {false, false};
  for (uint8_t c = 0; c < copies(); c++) {
    if (!readCopyHeader(slot, c, seq[c], crc[c], used[c])) seq[c] = 0;
  }
  // Newest first; a torn newest copy falls back to the other one.
  uint8_t order[2] = {0, 1};
  if (seq[1] > seq[0]) { order[0] = 1; order[1] = 0; }
  for (uint8_t i = 0; i < copies(); i++) {
    uint8_t c = order[i];
    if (seq[c] == 0) continue;
    if (!_io.read(_path, copyOffset(slot, c) + COPY_HEADER_SIZE, payload, _payload_size)) continue;
    if (crc16(crcStart(seq[c], used[c]), payload, _payload_size) == crc[c]) return used[c];
  }
  return false;
}

// Commit order: payload, rest of the copy header, and last the low byte of seq. The target copy's old seq always
// differs in that byte (seq never has a zero low byte, and in A/B the other copy is two behind), so until the final
// byte lands the stored seq disagrees with the CRC and the copy is invalid: a torn write keeps the previous value.
bool RecordFile::writeCopy(uint16_t slot, const uint8_t* payload) {
  uint8_t target = 0;
  uint32_t seq;
  uint8_t newest;
  uint32_t newest_seq;
  bool newest_used;
  bool have = newestCopy(slot, newest, newest_seq, newest_used);
  if (payload == nullptr && have && !newest_used) return true;   // already empty
  if (_ab) {
    if (have) target = (uint8_t)(1 - newest);
    seq = have ? newest_seq + 1 : 1;
  } else {
    uint16_t crc;
    bool used;
    readCopyHeader(slot, 0, seq, crc, used);
    seq++;
  }
  if ((seq & 0xFF) == 0) seq++;

  bool used = payload != nullptr;
  uint16_t crc = crcStart(seq, used);
  uint32_t off = copyOffset(slot, target);
  if (used) {
    crc = crc16(crc, payload, _payload_size);
    if (!_io.write(_path, off + COPY_HEADER_SIZE, payload, _payload_size)) return false;
  } else {
    static const uint8_t zeros[CHUNK] = {0};
    for (uint32_t done = 0; done < _payload_size;) {
      uint32_t n = _payload_size - done < CHUNK ? _payload_size - done : CHUNK;
      crc = crc16(crc, zeros, n);
      if (!_io.write(_path, off + COPY_HEADER_SIZE + done, zeros, n)) return false;
      done += n;
    }
  }
  uint8_t h[COPY_HEADER_SIZE];
  put32(h, seq);
  put16(&h[4], crc);
  h[6] = used ? 1 : 0;
  h[7] = 0;
  return _io.write(_path, off + 1, &h[1], sizeof(h) - 1) && _io.write(_path, off, h, 1);
}

bool RecordFile::write(uint16_t slot, const uint8_t* payload) {
  if (!_open || slot >= _slots || payload == nullptr) return false;
  return writeCopy(slot, payload);
}

bool RecordFile::erase(uint16_t slot) {
  if (!_open || slot >= _slots) return false;
  return writeCopy(slot, nullptr);
}

bool RecordFile::writeHeader(const char* path) const {
  uint8_t h[HEADER_SIZE] = {0};
  memcpy(h, MAGIC, 4);
  h[4] = _ver;
  h[5] = _ab ? 1 : 0;
  put16(&h[6], _payload_size);
  put16(&h[8], _slots);
  return _io.write(path, 0, h, sizeof(h));
}

bool RecordFile::readHeader(const char* path, Header& out) const {
  int32_t size = _io.size(path);
  uint8_t h[HEADER_SIZE];
  if (size < (int32_t)HEADER_SIZE || !_io.read(path, 0, h, sizeof(h))) return false;
  if (memcmp(h, MAGIC, 4) != 0 || h[5] > 1) return false;
  out.ver = h[4];
  out.ab = h[5] != 0;
  out.payload_size = get16(&h[6]);
  out.slots = get16(&h[8]);
  return (uint32_t)size == fileSize(out.payload_size, out.slots, out.ab);
}

bool RecordFile::matchesLayout(const char* path) const {
  Header h;
  return readHeader(path, h) && h.ver == _ver && h.ab == _ab && h.payload_size == _payload_size && h.slots == _slots;
}

RecordFile::Open RecordFile::createFresh() {
  if (!_io.create(_path, fileSize(_payload_size, _slots, _ab)) || !writeHeader(_path)) return Open::FAILED;
  _open = true;
  return Open::CREATED;
}

// FileIO has no rename, so a migration builds the new layout in "<path>.mig" (its header written last marks it
// complete), then copies it over the original (header last again) and removes it. A crash in the first phase leaves
// the original intact; a crash in the second is finished from the complete temporary file on the next open().
RecordFile::Open RecordFile::open(MigrateFn migrate_fn) {
  _open = false;
  char tmp[40];
  size_t n = strlen(_path);
  if (n + sizeof(TMP_SUFFIX) > sizeof(tmp)) return Open::FAILED;
  memcpy(tmp, _path, n);
  memcpy(&tmp[n], TMP_SUFFIX, sizeof(TMP_SUFFIX));

  if (_io.exists(tmp)) {
    if (!matchesLayout(_path) && matchesLayout(tmp)) {
      if (!finishMigration(tmp)) return Open::FAILED;
      _open = true;
      return Open::MIGRATED;
    }
    if (!_io.remove(tmp)) return Open::FAILED;
  }

  Header h;
  if (!_io.exists(_path) || !readHeader(_path, h)) return createFresh();
  if (h.ver == _ver && h.ab == _ab && h.payload_size == _payload_size && h.slots == _slots) {
    _open = true;
    return Open::OPENED;
  }
  // Same record format in another slot count or A/B mode (slot counts follow the free flash) needs no migrate fn.
  bool same_format = h.ver == _ver && h.payload_size == _payload_size;
  if (!same_format && migrate_fn == nullptr) return createFresh();
  return migrate(tmp, h, same_format ? nullptr : migrate_fn);
}

RecordFile::Open RecordFile::migrate(const char* tmp, const Header& old, MigrateFn fn) {
  if (old.payload_size > MAX_MIGRATE_PAYLOAD || _payload_size > MAX_MIGRATE_PAYLOAD) return createFresh();

  RecordFile src(_io, _path, old.ver, old.payload_size, old.slots, old.ab);
  src._open = true;
  RecordFile dst(_io, tmp, _ver, _payload_size, _slots, _ab);
  if (!_io.create(tmp, fileSize(_payload_size, _slots, _ab))) return Open::FAILED;
  dst._open = true;

  uint8_t in[MAX_MIGRATE_PAYLOAD], out[MAX_MIGRATE_PAYLOAD];
  uint16_t n = old.slots < _slots ? old.slots : _slots;
  for (uint16_t i = 0; i < n; i++) {
    if (!src.read(i, in)) continue;
    if (fn == nullptr) {
      memcpy(out, in, _payload_size);
    } else if (!fn(old.ver, in, old.payload_size, out)) {
      continue;   // unconvertible record: dropped like a CRC error (G7)
    }
    if (!dst.write(i, out)) return Open::FAILED;
  }
  if (!dst.writeHeader(tmp) || !finishMigration(tmp)) return Open::FAILED;
  _open = true;
  return Open::MIGRATED;
}

bool RecordFile::finishMigration(const char* tmp) {
  uint32_t size = fileSize(_payload_size, _slots, _ab);
  if (!_io.create(_path, size)) return false;
  uint8_t buf[CHUNK];
  for (uint32_t off = HEADER_SIZE; off < size;) {
    uint32_t n = size - off < CHUNK ? size - off : CHUNK;
    if (!_io.read(tmp, off, buf, n) || !_io.write(_path, off, buf, n)) return false;
    off += n;
  }
  return writeHeader(_path) && _io.remove(tmp);
}

}
