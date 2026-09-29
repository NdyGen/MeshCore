#pragma once

#include <stddef.h>
#include <stdint.h>

namespace rdm { namespace crypto {

// ACK_R: exactly the upstream formula (BaseChatMesh::composeMsgPacket): sha256(ts|flags|text, sender_pub)[0:4]
void ackR(uint8_t out[4], uint32_t ts, uint8_t flags, const char* text, size_t text_len, const uint8_t sender_pub[32]);
// ACK_S: SHA256("RDMS" | ts | (txt_type << 2) | text | sender_pub)[0:6]
void ackS(uint8_t out[6], uint32_t ts, uint8_t txt_type, const char* text, size_t text_len, const uint8_t sender_pub[32]);
// K: SHA256(ts | text | sender_pub)[0:4]
void key(uint8_t out[4], uint32_t ts, const char* text, size_t text_len, const uint8_t sender_pub[32]);
// token_B: HMAC-SHA256(K_owner, sender_pub)[0:8]
void tokenB(uint8_t out[8], const uint8_t k_owner[16], const uint8_t sender_pub[32]);
// ACK of a CTRL message: sha256(plaintext, sender_pub)[0:4]
void ctrlAck(uint8_t out[4], const uint8_t* plain, size_t len, const uint8_t sender_pub[32]);
// packet hash of a TXT_MSG payload, equal to mesh::Packet::calculatePacketHash (type 0x02)
void txtPacketHash(uint8_t out[8], const uint8_t* payload, size_t len);

}}
