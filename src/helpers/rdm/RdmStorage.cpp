#include "RdmStorage.h"

#include "RdmBytes.h"

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

// A copy whose header cannot be read counts as never written, like one with seq 0.
void RecordFile::readCopyHeaders(uint16_t slot, uint32_t seq[2], uint16_t crc[2], bool used[2]) const {
  for (uint8_t c = 0; c < 2; c++) {
    uint8_t h[COPY_HEADER_SIZE];
    seq[c] = 0;
    crc[c] = 0;
    used[c] = false;
    if (c < copies() && _io.read(_path, copyOffset(slot, c), h, sizeof(h))) {
      seq[c] = get32(h);
      crc[c] = get16(&h[4]);
      used[c] = h[6] != 0;
    }
  }
}

// Newest first; on equal seq copy 0 counts as the newest.
static void newestFirst(const uint32_t seq[2], uint8_t order[2]) {
  order[0] = seq[1] > seq[0] ? 1 : 0;
  order[1] = (uint8_t)(1 - order[0]);
}

// Continues crc over the stored payload: one read for payloads up to MAX_MIGRATE_PAYLOAD (every FileIO call is a
// file open on the device).
bool RecordFile::payloadCrc(uint16_t slot, uint8_t copy, uint16_t& crc) const {
  uint8_t buf[MAX_MIGRATE_PAYLOAD];
  uint32_t off = copyOffset(slot, copy) + COPY_HEADER_SIZE;
  for (uint32_t done = 0; done < _payload_size;) {
    uint32_t n = _payload_size - done < sizeof(buf) ? _payload_size - done : sizeof(buf);
    if (!_io.read(_path, off + done, buf, n)) return false;
    crc = crc16(crc, buf, n);
    done += n;
  }
  return true;
}

// The valid copy with the highest seq. Only that one is verified; the other only when it fails (a torn write).
bool RecordFile::newestCopy(uint16_t slot, const uint32_t seq[2], const uint16_t crc[2], const bool used[2],
                            uint8_t& copy) const {
  uint8_t order[2];
  newestFirst(seq, order);
  for (uint8_t i = 0; i < copies(); i++) {
    uint8_t c = order[i];
    if (seq[c] == 0) continue;
    uint16_t sum = crcStart(seq[c], used[c]);
    if (payloadCrc(slot, c, sum) && sum == crc[c]) {
      copy = c;
      return true;
    }
  }
  return false;
}

bool RecordFile::read(uint16_t slot, uint8_t* payload) const {
  if (!_open || slot >= _slots) return false;
  uint32_t seq[2];
  uint16_t crc[2];
  bool used[2];
  readCopyHeaders(slot, seq, crc, used);
  // Newest first; a torn newest copy falls back to the other one.
  uint8_t order[2];
  newestFirst(seq, order);
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
  uint32_t seq[2];
  uint16_t crc[2];
  bool used[2];
  readCopyHeaders(slot, seq, crc, used);
  uint8_t newest = 0;
  bool have = newestCopy(slot, seq, crc, used, newest);
  if (payload == nullptr && have && !used[newest]) return true;   // already empty

  uint8_t target = 0;
  uint32_t next;
  if (_ab) {
    if (have) target = (uint8_t)(1 - newest);
    next = have ? seq[newest] + 1 : 1;
  } else {
    next = seq[0] + 1;   // the header's seq, torn or not
  }
  if ((next & 0xFF) == 0) next++;

  bool now_used = payload != nullptr;
  uint16_t sum = crcStart(next, now_used);
  uint32_t off = copyOffset(slot, target);
  if (now_used) {
    sum = crc16(sum, payload, _payload_size);
    if (!_io.write(_path, off + COPY_HEADER_SIZE, payload, _payload_size)) return false;
  } else {
    static const uint8_t ZEROS[MAX_MIGRATE_PAYLOAD] = {0};
    for (uint32_t done = 0; done < _payload_size;) {
      uint32_t n = _payload_size - done < sizeof(ZEROS) ? _payload_size - done : sizeof(ZEROS);
      sum = crc16(sum, ZEROS, n);
      if (!_io.write(_path, off + COPY_HEADER_SIZE + done, ZEROS, n)) return false;
      done += n;
    }
  }
  uint8_t h[COPY_HEADER_SIZE];
  put32(h, next);
  put16(&h[4], sum);
  h[6] = now_used ? 1 : 0;
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
