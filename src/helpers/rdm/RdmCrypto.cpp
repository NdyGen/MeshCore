#include "RdmCrypto.h"

#include "RdmBytes.h"

#include <SHA256.h>

namespace rdm { namespace crypto {

static void putTs(SHA256& sha, uint32_t ts) {
  uint8_t b[4];
  put32(b, ts);
  sha.update(b, 4);
}

void ackR(uint8_t out[4], uint32_t ts, uint8_t flags, const char* text, size_t text_len, const uint8_t sender_pub[32]) {
  SHA256 sha;
  putTs(sha, ts);
  sha.update(&flags, 1);
  sha.update(text, text_len);
  sha.update(sender_pub, 32);
  sha.finalize(out, 4);
}

void ackS(uint8_t out[6], uint32_t ts, uint8_t txt_type, const char* text, size_t text_len, const uint8_t sender_pub[32]) {
  SHA256 sha;
  sha.update("RDMS", 4);
  putTs(sha, ts);
  uint8_t flags = (uint8_t)(txt_type << 2);
  sha.update(&flags, 1);
  sha.update(text, text_len);
  sha.update(sender_pub, 32);
  sha.finalize(out, 6);
}

void key(uint8_t out[4], uint32_t ts, const char* text, size_t text_len, const uint8_t sender_pub[32]) {
  SHA256 sha;
  putTs(sha, ts);
  sha.update(text, text_len);
  sha.update(sender_pub, 32);
  sha.finalize(out, 4);
}

void tokenB(uint8_t out[8], const uint8_t k_owner[16], const uint8_t sender_pub[32]) {
  SHA256 sha;
  sha.resetHMAC(k_owner, 16);
  sha.update(sender_pub, 32);
  sha.finalizeHMAC(k_owner, 16, out, 8);
}

void ctrlAck(uint8_t out[4], const uint8_t* plain, size_t len, const uint8_t sender_pub[32]) {
  SHA256 sha;
  sha.update(plain, len);
  sha.update(sender_pub, 32);
  sha.finalize(out, 4);
}

void txtPacketHash(uint8_t out[8], const uint8_t* payload, size_t len) {
  SHA256 sha;
  sha.update(&PAYLOAD_TYPE_TXT, 1);
  sha.update(payload, len);
  sha.finalize(out, 8);
}

}}
