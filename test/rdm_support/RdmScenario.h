#pragma once

// Assertion toolkit for the RDM scenario tests (06 par. 6.2): decodes every transmission of a simulation with the
// nodes' own keys, so a scenario can assert on what went over the air (RECEIPT_QUERY at +1 h, FETCH flood then
// PATH, ACK_R bytes equal to upstream) instead of on internal state. Also: read access to RDM record files in a
// node's flash, and the 20% airtime norm.

#include <Sim.h>
#include <SimFS.h>
#include <helpers/rdm/RdmTypes.h>

#include <gtest/gtest.h>

#include <stdint.h>

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace sim {

enum class Kind : uint8_t {
  UNKNOWN, ADVERT, DM, CTRL, ACK, PATH,
  QUERY, QUERY_RESP, DEPOSIT, DEPOSIT_RESP, FETCH, FETCH_RESP, STATUS, STATUS_RESP, REG, REG_RESP,
  REQ_OTHER, RESP_OTHER
};
const char* kindName(Kind k);
const char* statusName(rdm::UserStatus s);
std::string toString(const std::vector<rdm::UserStatus>& v);

// One transmission, decoded. Forwarded copies (repeater) are separate records with the same origin.
struct Wire {
  uint64_t seq = 0, t_ms = 0;
  uint32_t airtime_ms = 0;
  int      tx_node = -1;           // transmitter
  int      origin = -1;            // node that created the packet (first transmitter of this packet hash)
  int      dest = -1;              // addressed node (datagrams, ANON_REQ); -1 for ACK and ADVERT
  uint8_t  type = 0;
  bool     flood = false;
  uint8_t  hops = 0;               // path hash count in the header
  uint8_t  pkt_hash[8] = {};
  Kind     kind = Kind::UNKNOWN;
  Kind     inner = Kind::UNKNOWN;  // PATH: what its extra carries (ACK or a response kind)
  bool     decrypted = false;
  uint32_t ts = 0;                 // TXT/REQ/ANON_REQ: sender timestamp; responses (also inside PATH): tag
  std::vector<uint8_t> body;       // REQ: from req_type on; response: after the tag; TXT/ANON_REQ: whole plaintext
  std::vector<uint8_t> ack;        // ACK payload, or the ACK extra of a PATH (at most 7 bytes, padding cut)
  // TXT_MSG (DM, CTRL)
  std::string text;
  uint8_t  txt_type = 0, attempt = 0;
  bool     ext_attempt = false, cap = false;

  bool original() const { return tx_node == origin; }
  Kind effective() const { return kind == Kind::PATH && inner != Kind::UNKNOWN ? inner : kind; }
};

class WireLog {
public:
  explicit WireLog(Simulator& sim) : _sim(sim) {}
  // Nodes whose keys decode traffic (every node that sends or receives datagrams in the scenario).
  void addNode(SimNode& n);
  // Decodes records added to the simulator's tx_log since the last call.
  const std::vector<Wire>& all();
  std::vector<Wire> select(const std::function<bool(const Wire&)>& pred);
  // Original transmissions of `origin` with this (effective) kind in [from_ms, to_ms).
  std::vector<Wire> sent(const SimNode& origin, Kind k, uint64_t from_ms = 0, uint64_t to_ms = UINT64_MAX);
  // Sum of airtime (every transmission, forwarded copies included) matching pred.
  uint64_t airtime(const std::function<bool(const Wire&)>& pred);
  std::string describe(const Wire& w) const;
  std::string dump(uint64_t from_ms = 0, uint64_t to_ms = UINT64_MAX);

private:
  struct PendingReq { uint32_t tag; int requester, responder; Kind kind; };
  Simulator& _sim;
  std::vector<SimNode*> _nodes;
  std::vector<Wire> _wires;
  size_t _decoded = 0;
  std::map<std::string, int> _first_tx;                   // packet hash -> first transmitter
  std::map<std::pair<int, int>, std::vector<uint8_t>> _secrets;
  std::vector<PendingReq> _reqs;

  Wire decode(const TxRecord& tx);
  bool decryptDatagram(Wire& w, const uint8_t* payload, size_t len, std::vector<uint8_t>& plain);
  const std::vector<uint8_t>& secret(int a, int b);
  int nodeByPub(const uint8_t* pub) const;
  Kind classifyResponse(uint32_t tag, int requester, int responder) const;
  void classifyPlain(Wire& w, const std::vector<uint8_t>& plain);
};

// Upstream and RDM proofs of a plain DM, for comparing with what went over the air.
uint32_t expectAckR(uint32_t ts, uint8_t attempt, const std::string& text, const uint8_t sender_pub[32]);
std::vector<uint8_t> expectAckS(uint32_t ts, const std::string& text, const uint8_t sender_pub[32]);
uint32_t ackValue(const std::vector<uint8_t>& ack);   // first 4 bytes, little-endian like the firmware's uint32_t

// RDM record files (rdm::RecordFile format) in a node's flash, read from a copy so the node is not disturbed.
struct RecordDump {
  bool ok = false;
  uint16_t payload_size = 0, slots = 0;
  std::vector<std::vector<uint8_t>> records;   // payloads of the used slots, slot order
};
RecordDump readRecords(const SimFS& fs, const char* path);
struct ContactRdmView { uint8_t flags; uint32_t watermark; uint8_t mbx_pub[32]; uint8_t token[8]; };
bool readContactRdm(const SimFS& fs, const uint8_t pub_prefix[6], ContactRdmView& out);
bool fileContains(const SimFS& fs, const char* path, const std::string& needle);

// 06 par. 6.2: airtime per hop within 20% of the figure in 03 par. 5 or 05.
::testing::AssertionResult airtimeWithin(uint64_t measured_ms, double expected_s, double tolerance = 0.20);

}
