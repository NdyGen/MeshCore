#if defined(ARDUINO) || defined(RDM_SIM_FS)

#include "ArduinoFileIO.h"

#if defined(ESP32)
  #include <SPIFFS.h>
#elif defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  using namespace Adafruit_LittleFS_Namespace;
#endif

#include <string.h>

namespace rdm {

static const uint32_t ZERO_CHUNK = 64;

static File openRead(RDM_FILESYSTEM& fs, const char* path) {
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  return fs.open(path, FILE_O_READ);
#else
  return fs.open(path, "r");
#endif
}

// Read/write without truncation; the position is set with seek() afterwards.
static File openUpdate(RDM_FILESYSTEM& fs, const char* path) {
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  return fs.open(path, FILE_O_WRITE);
#else
  return fs.open(path, "r+");
#endif
}

static File openTruncate(RDM_FILESYSTEM& fs, const char* path) {
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  fs.remove(path);   // FILE_O_WRITE would append to an existing file
  return fs.open(path, FILE_O_WRITE);
#else
  return fs.open(path, "w");
#endif
}

#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
// The traversal API changed in littlefs v2 (lfs_fs_traverse, no cfg->block_count on the handle).
static_assert(LFS_VERSION_MAJOR == 1, "ArduinoFileIO::fsSpace walks the blocks with the littlefs v1 API");

static int countBlock(void* p, lfs_block_t block) {
  (void)block;
  *(lfs_size_t*)p += 1;
  return 0;
}
#endif

bool ArduinoFileIO::exists(const char* path) {
  return _fs.exists(path);
}

int32_t ArduinoFileIO::size(const char* path) {
  if (!_fs.exists(path)) return -1;
  File f = openRead(_fs, path);
  if (!f) return -1;
  int32_t n = (int32_t)f.size();
  f.close();
  return n;
}

bool ArduinoFileIO::create(const char* path, uint32_t size) {
  int32_t existing = this->size(path);
  if ((uint64_t)size > (uint64_t)freeBytes() + (existing > 0 ? (uint32_t)existing : 0)) return false;

  // LittleFS needs the directory; on SPIFFS (flat names) mkdir is a harmless no-op.
  const char* slash = strrchr(path, '/');
  if (slash && slash != path) {
    char dir[40];
    size_t n = (size_t)(slash - path);
    if (n < sizeof(dir)) {
      memcpy(dir, path, n);
      dir[n] = 0;
      if (!_fs.exists(dir)) _fs.mkdir(dir);
    }
  }

  File f = openTruncate(_fs, path);
  if (!f) return false;
  static const uint8_t zeros[ZERO_CHUNK] = {0};
  bool ok = true;
  for (uint32_t done = 0; ok && done < size;) {
    uint32_t n = size - done < ZERO_CHUNK ? size - done : ZERO_CHUNK;
    ok = f.write(zeros, n) == n;
    done += n;
  }
  f.close();
  return ok && this->size(path) == (int32_t)size;
}

bool ArduinoFileIO::read(const char* path, uint32_t off, uint8_t* buf, uint32_t len) {
  File f = openRead(_fs, path);
  if (!f) return false;
  bool ok = (uint64_t)off + len <= f.size() && f.seek(off) && (uint32_t)f.read(buf, len) == len;
  f.close();
  return ok;
}

bool ArduinoFileIO::write(const char* path, uint32_t off, const uint8_t* buf, uint32_t len) {
  if (!_fs.exists(path)) return false;
  File f = openUpdate(_fs, path);
  if (!f) return false;
  bool ok = (uint64_t)off + len <= f.size() && f.seek(off) && f.write(buf, len) == len;
  f.close();
  return ok;
}

bool ArduinoFileIO::remove(const char* path) {
  return _fs.remove(path);
}

uint32_t ArduinoFileIO::freeBytes() {
  uint32_t total, used;
  bool ok = _space ? _space(total, used) : fsSpace(total, used);
  return ok && used < total ? total - used : 0;
}

bool ArduinoFileIO::fsSpace(uint32_t& total, uint32_t& used) {
#if defined(ESP32)
  // fs::FS has no size query; without a SpaceFn the board's only FS (SPIFFS) is assumed, as before AR7
  total = (uint32_t)SPIFFS.totalBytes();
  used = (uint32_t)SPIFFS.usedBytes();
  return true;
#elif defined(RP2040_PLATFORM)
  FSInfo info;
  if (!_fs.info(info)) return false;
  total = (uint32_t)info.totalBytes;
  used = (uint32_t)info.usedBytes;
  return true;
#else
  const lfs_config* cfg = _fs._getFS()->cfg;
  lfs_size_t blocks = 0;
  if (lfs_traverse(_fs._getFS(), countBlock, &blocks) != 0) return false;
  total = (uint32_t)(cfg->block_count * cfg->block_size);
  used = (uint32_t)(blocks * cfg->block_size);
  return true;
#endif
}

}

#endif
