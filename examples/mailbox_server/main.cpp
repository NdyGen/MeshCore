#include <Arduino.h>   // needed for PlatformIO
#include <Mesh.h>

#if defined(NRF52_PLATFORM)
  #include <InternalFileSystem.h>
#elif defined(RP2040_PLATFORM)
  #include <LittleFS.h>
#elif defined(ESP32)
  #include <SPIFFS.h>
#endif

#include <helpers/ArduinoHelpers.h>
#include <helpers/IdentityStore.h>
#include <helpers/SimpleMeshTables.h>
#include <helpers/StaticPoolPacketManager.h>
#include <target.h>

#include "MailboxMesh.h"
#include "SerialPiBackend.h"

#ifndef FIRMWARE_VERSION
  #define FIRMWARE_VERSION   "v1.17.1-mbx1"
#endif
#ifndef ADVERT_NAME
  #define ADVERT_NAME   "Mailbox"
#endif
// Radio settings come from the build (there is no CLI: the serial port belongs to mbxd).
#ifndef LORA_FREQ
  #define LORA_FREQ   915.0
#endif
#ifndef LORA_BW
  #define LORA_BW     250
#endif
#ifndef LORA_SF
  #define LORA_SF     10
#endif
#ifndef LORA_CR
  #define LORA_CR      5
#endif
#ifndef LORA_TX_POWER
  #define LORA_TX_POWER  20
#endif

StdRNG fast_rng;
SimpleMeshTables tables;
StaticPoolPacketManager packet_pool(32);
ArduinoMillis millis_clock;
SerialPiBackend pi(Serial, millis_clock);
MailboxMesh the_mesh(radio_driver, millis_clock, fast_rng, rtc_clock, packet_pool, tables, pi, ADVERT_NAME);

static void halt() {
  while (1) ;
}

#ifdef DISPLAY_CLASS
static unsigned long next_screen = 0;

static void showStatus() {
  if (millis() < next_screen) return;
  next_screen = millis() + 2000;
  char line[32];
  display.startFrame();
  display.setCursor(0, 0);
  display.print(ADVERT_NAME);
  display.setCursor(0, 12);
  snprintf(line, sizeof(line), "ID %02X%02X%02X%02X", the_mesh.self_id.pub_key[0], the_mesh.self_id.pub_key[1],
           the_mesh.self_id.pub_key[2], the_mesh.self_id.pub_key[3]);
  display.print(line);
  display.setCursor(0, 24);
  display.print(pi.ready() ? "Pi: ready" : "Pi: waiting");
  display.setCursor(0, 36);
  snprintf(line, sizeof(line), "Peers: %u", (unsigned)the_mesh.peerCount());
  display.print(line);
  display.endFrame();
}
#endif

void setup() {
#ifdef ESP32
  Serial.setRxBufferSize(2048);   // mbxd sends up to 64 ACL lines in one burst after HELLO
#endif
  Serial.begin(115200);
  delay(1000);

  board.begin();

#ifdef DISPLAY_CLASS
  if (display.begin()) {
    display.startFrame();
    display.setCursor(0, 0);
    display.print("Please wait...");
    display.endFrame();
  }
#endif

  if (!radio_init()) { halt(); }
  radio_driver.setParams(LORA_FREQ, LORA_BW, LORA_SF, LORA_CR);
  radio_driver.setTxPower(LORA_TX_POWER);

  fast_rng.begin(radio_driver.getRngSeed());

#if defined(NRF52_PLATFORM)
  InternalFS.begin();
  IdentityStore store(InternalFS, "");
#elif defined(RP2040_PLATFORM)
  LittleFS.begin();
  IdentityStore store(LittleFS, "/identity");
  store.begin();
#elif defined(ESP32)
  SPIFFS.begin(true);
  IdentityStore store(SPIFFS, "/identity");
#else
  #error "need to define filesystem"
#endif
  if (!store.load("_main", the_mesh.self_id)) {
    the_mesh.self_id = radio_new_identity();
    int count = 0;
    while (count < 10 && (the_mesh.self_id.pub_key[0] == 0x00 || the_mesh.self_id.pub_key[0] == 0xFF)) {  // reserved id hashes
      the_mesh.self_id = radio_new_identity(); count++;
    }
    store.save("_main", the_mesh.self_id);
  }

  // Not an mbx. line, so mbxd ignores it; useful when setting up the owners.
  Serial.print("Mailbox ID: ");
  mesh::Utils::printHex(Serial, the_mesh.self_id.pub_key, PUB_KEY_SIZE);
  Serial.println();

  the_mesh.begin();
  pi.begin(FIRMWARE_VERSION);
  board.onBootComplete();
}

void loop() {
  the_mesh.loop();
  rtc_clock.tick();
  board.loop();
#ifdef DISPLAY_CLASS
  showStatus();
#endif
}
