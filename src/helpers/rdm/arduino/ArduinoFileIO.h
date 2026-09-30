#pragma once

#if defined(ARDUINO) || defined(RDM_SIM_FS)

#include <helpers/rdm/RdmStorage.h>

#if defined(ESP32) || defined(RP2040_PLATFORM)
  #include <FS.h>
  #define RDM_FILESYSTEM fs::FS
#elif defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  #include <Adafruit_LittleFS.h>
  #define RDM_FILESYSTEM Adafruit_LittleFS
#else
  #error "ArduinoFileIO: unsupported platform"
#endif

namespace rdm {

// rdm::FileIO on the companion's flash filesystem (SPIFFS on ESP32, LittleFS on RP2040/nRF52/STM32).
// Records are updated in place with seek + write; openWrite() in DataStore would remove the file first on nRF52.
class ArduinoFileIO : public FileIO {
public:
  // Capacity of the filesystem, for freeBytes(). fs::FS on ESP32 cannot report it, but the caller that chose the FS
  // knows which object can (SPIFFS); without one RP2040 asks _fs.info() and nRF52 walks the littlefs blocks.
  using SpaceFn = bool (*)(uint32_t& total, uint32_t& used);

  explicit ArduinoFileIO(RDM_FILESYSTEM& fs, SpaceFn space = nullptr) : _fs(fs), _space(space) {}

  bool     exists(const char* path) override;
  int32_t  size(const char* path) override;
  bool     create(const char* path, uint32_t size) override;
  bool     read(const char* path, uint32_t off, uint8_t* buf, uint32_t len) override;
  bool     write(const char* path, uint32_t off, const uint8_t* buf, uint32_t len) override;
  bool     remove(const char* path) override;
  uint32_t freeBytes() override;

private:
  RDM_FILESYSTEM& _fs;
  SpaceFn _space;

  bool fsSpace(uint32_t& total, uint32_t& used);
};

}

#endif
