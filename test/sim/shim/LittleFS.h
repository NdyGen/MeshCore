#pragma once

#include <FS.h>

// Firmware built for RP2040_PLATFORM calls the global LittleFS (e.g. DataStore::formatFileSystem()).
extern fs::CurrentNodeFS LittleFS;
