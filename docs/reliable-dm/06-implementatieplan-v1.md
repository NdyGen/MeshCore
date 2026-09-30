# 06: Implementatieplan v1

Doel (Andy, 29 sep 2026): een eerste versie geïmplementeerd, met unit- en integratietests die bewijzen dat het werkt.
Leidend ontwerp: `03-ontwerp.md` (inclusief par. 11, G1-G15) en `adr-001-reliable-dm.md`. Scenario's: `05-sequenties.md`. Simulator: `07-simulator.md` (worker dm-current).
Dit document legt vast wat parallelle workers nodig hebben: architectuur, headers en wire-formaten die niet meer onderhandeld worden, eigendom per bestand, en de testmatrix. Wijzigingen aan een vastgelegde header lopen via de tech lead en worden hier bijgewerkt.

## 1. Scope v1

| fase | onderdeel | in v1 |
|---|---|---|
| A | outbox (persistent, app-retries op één entry, firmware-attempts, probes, T_radio/T_sync, receipt-watch) | ja |
| A | inbox en register (ACK na opslag, #3518, dedup over attempts, quotum, watermerk, sync-detectie) | ja |
| A | cap-trailer en cap-ACK, `ACK_R`/`ACK_S`, `RECEIPT_QUERY` en probe | ja |
| A | companion: 0x91, `CMD_RDM_*`, `SEND_CONFIRMED` bij `ACK_R`, statuskanaal achter `RDM_STATUS_CHANNEL` | ja |
| B | mailbox-firmware `examples/mailbox_server` en `mbxd` op de Pi | ja |
| B | MBX_INFO met `token_B` (HMAC), registratie, quotum, denylist, DEPOSIT/FETCH/STATUS, RESYNC via `store_id` | ja |
| - | upstream-PR, GitHub-discussie, #3518-PR indienen | nee (beslissing 5) |
| - | HIL op hardware | na v1-merge, zie par. 8 |

Alles achter build-flags. Zonder flag zijn machinecode, data en preprocessor-uitvoer identiek aan upstream; alleen regelnummers in de debug-info (en op ESP32 de daarvan afgeleide ELF- en image-hash in `firmware.bin`) mogen verschuiven, omdat `#ifdef`-blokken regels toevoegen. WP10 bewijst dat (`check-upstream-identical.sh`: IDENTICAL of CODE-IDENTICAL).

| flag | waar | effect |
|---|---|---|
| `WITH_RELIABLE_DM` | companion, `src/Mesh.cpp`, `src/helpers/BaseChatMesh.*` | fase A plus mailbox-client |
| `RDM_STATUS_CHANNEL` | companion, alleen samen met `WITH_RELIABLE_DM` | lokaal statuskanaal voor de standaard app |
| `WITH_DM_MAILBOX` | `examples/mailbox_server`, `src/Mesh.cpp` | mailboxrol |

## 2. Architectuur

```
            companion_radio/MyMesh          test: RdmSimCompanion (op sim::SimNode)
                     |  (erft, alleen met flag)          |
                     v                                   v
             src/helpers/rdm/RdmChatMesh  (BaseChatMesh-subklasse: packets <-> rdm::Node)
                     |
                     v
             rdm::Node  (geen Arduino, geen radio)
      +--------+--------+---------+----------+
      Outbox   Inbox    Fetcher   ContactTable  Clock
      +--------+--------+---------+----------+
             Codec  Crypto  RecordFile --> FileIO (interface)
                                             | device: ArduinoFileIO (SPIFFS/LittleFS)
                                             | test:   MemFileIO (foutinjectie), SimFileIO (sim::SimFS)

 examples/mailbox_server/MailboxMesh  --->  rdm::MailboxCore  --->  MailboxBackend (interface)
                                                                     | device: SerialPiBackend <-> mbxd (Python, Pi)
                                                                     | test:   MemMailboxBackend (C++), SubprocessBackend (echte mbxd)
```

Principes:
- **Protocol-logica is pure C++** in `src/helpers/rdm/`: geen `Arduino.h`, geen `millis()`, geen bestandssysteem, geen radio. Tijd komt binnen als argument (RDM-tijd in seconden, par. 11 G6 van `03`), opslag via `rdm::FileIO`, zenden via host-interfaces.
- **Eén integratielaag voor device en simulator**: `RdmChatMesh` vertaalt tussen packets en `rdm::Node`. De companion en de simulator gebruiken dezelfde klasse; alleen de app-kant (frames naar de telefoon) verschilt.
- **Minimale ingreep in upstream-bestanden**: twee protected helpers in `BaseChatMesh` en de exacte lengtecheck in `Mesh::createDatagram`, allebei onder flag. De rest gaat via bestaande virtuele methoden (`onPeerDataRecv`, `onPeerPathRecv`, `onAckRecv`, `onContactPathRecv`, `searchPeersByHash`, `getPeerSharedSecret`).
- **Mailbox-semantiek op één plek gespecificeerd**: `mbxd` (Python) is bron van waarheid op het device; `MemMailboxBackend` (C++) implementeert dezelfde regels voor de simulator. Beide draaien dezelfde conformance-vectoren (`test/rdm_vectors/mailbox_conformance.json`).

## 3. Vastgelegde interfaces

Recordgroottes gelden voor de gepakte serialisatie: implementaties schrijven records veld voor veld (little-endian), nooit via `memcpy` van een struct, zodat compiler-padding het bestandsformaat niet raakt. Elke module die een bestand bezit, exporteert de recordgrootte en het pad in zijn header (`RECORD_SIZE`, `PATH`; bij twee bestanden ook `WATCH_*`/`REG_*`, bij `Node` `MBX_*`); `Node` plant en opent de bestanden daarmee, de module controleert in `begin` de `payloadSize()` ertegen.

Kernheaders in `src/helpers/rdm/`, de integratielaag in `mesh/`, de platformadapter in `arduino/` en de serverrol in `mailbox/` (par. 4); namespace `rdm`, `#pragma once`, alleen `<stdint.h>`, `<stddef.h>`, `<string.h>` en andere rdm-headers als include (uitzondering: `mesh/RdmChatMesh.h`, `arduino/ArduinoFileIO.h`). Headers in een submap includen als `<helpers/rdm/...>`, zoals upstream. `RdmBytes.h` (little-endian `put16/get16/put32/get32`, `isZero`) en `RdmPolicy.h` (`jitterMax`, `jittered`, `firstRetryDelay`, `waitSecs`) zijn interne hulpheaders van de kern en staan niet in deze paragraaf. Little-endian op de wire. WP0 schrijft deze headers exact zoals hieronder; implementerende WP's wijzigen de public API en de struct-velden niet. Het `private`-deel van een klasse bevat in de headers alleen de referenties uit de constructor (en bij `Node` de host-overrides, zie 3.11); de implementerende WP mag daar leden aan toevoegen. Codecommentaar in de headers is Engels, zoals in de rest van de codebase.

### 3.1 `RdmTypes.h`

```cpp
namespace rdm {

constexpr uint8_t TXT_TYPE_RDM_CTRL     = 8;
constexpr uint8_t CAP_BYTE              = 0x81;
constexpr uint8_t CTRL_MBX_INFO         = 0x01;
constexpr uint8_t CTRL_MBX_REVOKE       = 0x02;
constexpr uint8_t CTRL_VERSION          = 1;
constexpr uint8_t REQ_DEPOSIT           = 0x41;
constexpr uint8_t REQ_FETCH             = 0x42;
constexpr uint8_t REQ_STATUS            = 0x44;
constexpr uint8_t REQ_RECEIPT_QUERY     = 0x45;
constexpr uint8_t REG_MARKER0           = 0xFF;
constexpr uint8_t REG_MARKER1           = 'M';
constexpr uint8_t REG_VERSION           = 1;
constexpr uint8_t FETCH_FLAG_NO_PAYLOAD = 0x02;
constexpr uint8_t FW_ATTEMPT_BASE       = 252;   // + klasse, aflopend in stappen van 4 (03 par. 1d)
constexpr size_t  MAX_TEXT              = 157;   // fork-DM met trailer
constexpr size_t  INBOX_TEXT_MAX        = 160;   // upstream MAX_TEXT_LEN: standaard afzenders mogen dat gebruiken
constexpr size_t  MAX_TEXT_MAILBOX      = 152;   // mailboxkopie
constexpr size_t  MAX_INNER_PAYLOAD     = 164;   // dest|src|MAC|ciphertext van een mailboxkopie
constexpr size_t  MAX_BATCH             = 8;

enum class OutState : uint8_t {
  NEW = 0, WAIT_ACK = 1, RETRY = 2, DEPOSITING = 3, REGISTER = 4, CUSTODY = 5,
  ON_RADIO = 6, ON_RADIO_FINAL = 7, DELIVERED = 8, REJECTED = 9, EXPIRED = 10, SYNC_EXPIRED = 11
};
enum class UserStatus : uint8_t {        // waarde in PUSH_CODE_RDM_STATUS
  QUEUED = 0, CUSTODY = 1, ON_RADIO = 2, DELIVERED = 3, ON_RADIO_FINAL = 4,
  REJECTED = 5, EXPIRED = 6, SYNC_EXPIRED = 7, TOO_BIG = 8, NO_OUTBOX = 9
};
enum class QueryState   : uint8_t { UNKNOWN = 0, ON_RADIO = 1, SYNCED = 2, EVICTED = 3 };
enum class ReportResult : uint8_t { ON_RADIO = 0, SYNCED = 1, DUPLICATE = 2, UNDECRYPTABLE = 3, INBOX_FULL = 4 };
enum class MbxState     : uint8_t { UNKNOWN = 0, STORED = 1, ON_RADIO = 2, DELIVERED = 3, REJECTED = 4, EXPIRED = 5, SYNC_EXPIRED = 6 };
enum class MbxCode      : uint8_t { OK = 0x00, ALREADY_STORED = 0x01, NOT_AUTH = 0x10, UNKNOWN_OWNER = 0x11,
                                    QUOTA = 0x12, NO_STORAGE = 0x13, TOO_BIG = 0x14, RATE_LIMITED = 0x15 };
enum class RecvResult   : uint8_t { NEW = 0, DUPLICATE = 1, INBOX_FULL = 2, STORE_ERROR = 3 };

struct QueryItem   { uint32_t ts; uint8_t key[4]; };
struct QueryReply  { QueryState state; uint8_t ack[6]; };      // ON_RADIO: ACK_R + 2 nullen; SYNCED: ACK_S
struct Report      { uint8_t pkt_hash[8]; ReportResult result; uint8_t ack[6]; };
struct StatusReply { MbxState state; uint8_t ack[6]; };

inline bool sameReport(const Report& a, const Report& b) {
  return a.result == b.result && memcmp(a.pkt_hash, b.pkt_hash, 8) == 0 && memcmp(a.ack, b.ack, 6) == 0;
}

inline UserStatus toUserStatus(OutState s) {                   // 03 par. 11, G1
  switch (s) {
    case OutState::CUSTODY:        return UserStatus::CUSTODY;
    case OutState::ON_RADIO:       return UserStatus::ON_RADIO;
    case OutState::ON_RADIO_FINAL: return UserStatus::ON_RADIO_FINAL;
    case OutState::DELIVERED:      return UserStatus::DELIVERED;
    case OutState::REJECTED:       return UserStatus::REJECTED;
    case OutState::EXPIRED:        return UserStatus::EXPIRED;
    case OutState::SYNC_EXPIRED:   return UserStatus::SYNC_EXPIRED;
    default:                       return UserStatus::QUEUED;
  }
}
inline bool isFinal(OutState s) { return s >= OutState::ON_RADIO_FINAL; }

}
```

### 3.2 `RdmConfig.h`

Slotmaxima, `RDM_MIN_FREE_BYTES` en `RDM_MBX_CLIENTS` zijn `#ifndef`-overschrijfbaar (tests, kleine borden). Tijden in seconden RDM-tijd. Parameters die aan twee kanten van een contract gelden (client en mailbox) staan hier samen, met een `static_assert` op hun verhouding.

```cpp
#ifndef RDM_OUTBOX_SLOTS_MAX
  #define RDM_OUTBOX_SLOTS_MAX    16
#endif
#ifndef RDM_REGISTER_SLOTS_MAX
  #define RDM_REGISTER_SLOTS_MAX  512
#endif
#ifndef RDM_INBOX_SLOTS_MAX
  #define RDM_INBOX_SLOTS_MAX     256
#endif
#ifndef RDM_WATCH_SLOTS_MAX
  #define RDM_WATCH_SLOTS_MAX     128
#endif
#ifndef RDM_CONTACT_SLOTS_MAX
  #define RDM_CONTACT_SLOTS_MAX   64
#endif
#ifndef RDM_MIN_FREE_BYTES
  #define RDM_MIN_FREE_BYTES      16384    // minder vrij: RDM uit (03 par. 6)
#endif
#define RDM_HIDDEN_PEERS          4
#define RDM_T_RADIO_S             (7UL * 86400)
#define RDM_T_SYNC_S              (30UL * 86400)
#define RDM_WATCH_S               (90UL * 86400)
#define RDM_FINAL_KEEP_S          86400        // G2, default D3
#define RDM_CUSTODY_GRACE_S       86400        // G10
#define RDM_T_MAX_CUSTODY_S       (30UL * 86400)   // G10: ttl_s van M, hooguit 30 dagen
#define RDM_RETRY_AFTER_LOSS_S    86400        // besluit WP3: minstens een dag extra nadat M of Alice de kopie verloor
#define RDM_WAIT_ACK_MIN_S        30           // G3: max(2 x est_timeout, 30 s)
#define RDM_OUTBOX_TX_GAP_S       60
#define RDM_BOOT_KICK_MIN_S       60           // G6
#define RDM_BOOT_KICK_MAX_S       120
#define RDM_CLOCK_PERSIST_S       600
#define RDM_HEARD_MIN_S           3600         // hear-trigger in RETRY/ON_RADIO/CUSTODY
#define RDM_MBX_ADVERT_MIN_S      600
#define RDM_FETCH_INTERVAL_S      3600
#define RDM_FETCH_BOOT_JITTER_S   60
#define RDM_MBX_REQ_GAP_S         6            // min. tijd tussen REQ's naar één mailbox: de gap van de server + 1 s
#define RDM_MBX_SERVER_REQ_GAP_S  5            // MailboxCore accepteert 1 REQ per 5 s per client (03 par. 5)
#ifndef RDM_MBX_CLIENTS
  #define RDM_MBX_CLIENTS         64           // clients die een mailbox bijhoudt: peer-cache en rate-limits van MailboxCore
#endif
#define RDM_INBOX_QUOTA_DIV       4            // max 1/4 van de inbox per afzender (K8)
// schema's (03 par. 5), in seconden na het vorige ijkpunt; laatste waarde herhaalt
#define RDM_SCHED_DM_BACKOFF      { 300, 900, 3600, 14400, 43200, 86400 }
#define RDM_SCHED_PROBE_DAY1      1800         // eerste 24 u
#define RDM_SCHED_PROBE_LATER     7200
#define RDM_PROBE_FLOOD_MIN_S     14400
#define RDM_DM_EVEN_WITH_CAP_S    86400        // G15
#define RDM_SCHED_ON_RADIO        { 3600, 14400, 43200, 86400 }
#define RDM_SCHED_STATUS          { 600, 1800, 3600, 14400, 43200 }
#define RDM_SCHED_REDEPOSIT       { 600, 1800, 3600, 14400, 43200 }   // G13
#define RDM_CUSTODY_PROBE_S       86400
#define RDM_RETRY_FIRST_MIN_S     60           // G16: eerste RETRY-actie 60-120 s na de time-out
#define RDM_RETRY_FIRST_MAX_S     120
#define RDM_JITTER_MAX_S          60           // G17: elke herhalende tussenpoos I wordt I + U[0, min(I / RDM_JITTER_DIV, RDM_JITTER_MAX_S)]
#define RDM_JITTER_DIV            10

static_assert(RDM_MBX_REQ_GAP_S > RDM_MBX_SERVER_REQ_GAP_S, "a client REQ must never hit the mailbox's rate limit");
```

### 3.3 `RdmCrypto.h`

Implementatie op `mesh::Utils::sha256` en de `SHA256`-klasse uit rweather/Crypto; in `native_rdm` is dat de echte bibliotheek.

```cpp
namespace rdm { namespace crypto {
// ACK_R: exact de upstream-formule (BaseChatMesh::composeMsgPacket): sha256(ts|flags|text, pub_afzender)[0:4]
void ackR(uint8_t out[4], uint32_t ts, uint8_t flags, const char* text, size_t text_len, const uint8_t sender_pub[32]);
// ACK_S: SHA256("RDMS" | ts | (txt_type << 2) | text | pub_afzender)[0:6]
void ackS(uint8_t out[6], uint32_t ts, uint8_t txt_type, const char* text, size_t text_len, const uint8_t sender_pub[32]);
// K: SHA256(ts | text | pub_afzender)[0:4]
void key(uint8_t out[4], uint32_t ts, const char* text, size_t text_len, const uint8_t sender_pub[32]);
// token_B: HMAC-SHA256(K_owner, pub_afzender)[0:8]
void tokenB(uint8_t out[8], const uint8_t k_owner[16], const uint8_t sender_pub[32]);
// ACK van een CTRL-bericht: sha256(plaintext, pub_afzender)[0:4]
void ctrlAck(uint8_t out[4], const uint8_t* plain, size_t len, const uint8_t sender_pub[32]);
// packet hash van een TXT_MSG-payload, gelijk aan mesh::Packet::calculatePacketHash (type 0x02)
void txtPacketHash(uint8_t out[8], const uint8_t* payload, size_t len);
}}
```

### 3.4 `RdmCodec.h`

Alle `build*`-functies geven het aantal geschreven bytes (0 = fout). Alle `parse*`-functies accepteren achterliggende nulpadding (ontsleutelde data is tot een blok van 16 aangevuld) en weigeren te korte of inconsistente input. REQ-bodies beginnen bij `req_type` (de 4-byte timestamp zet de adapter ervoor); RESPONSE-bodies beginnen na de 4-byte tag.

```cpp
namespace rdm { namespace codec {

// TXT-plaintext: ts(4) | flags(1) | tekst | [0x00 | attempt | CAP_BYTE]
size_t buildTxtPlain(uint8_t* out, uint32_t ts, uint8_t txt_type, uint8_t attempt,
                     const char* text, size_t text_len, bool cap_trailer);
struct TxtParsed {
  uint32_t ts; uint8_t flags; uint8_t txt_type;
  const char* text; size_t text_len;
  bool has_ext_attempt; uint8_t ext_attempt;   // byte na de NUL (upstream-precedent)
  bool cap;                                    // tweede byte na de NUL == CAP_BYTE
};
bool parseTxtPlain(const uint8_t* data, size_t len, TxtParsed& out);

// ACK-bytes: ack_r(4) | ext_attempt(1) | random(1) [| CAP_BYTE]   -> 6 of 7 bytes
size_t buildAckR(uint8_t out[7], const uint8_t ack_r[4], uint8_t ext_attempt, uint8_t rnd, bool cap);
bool   ackHasCap(const uint8_t* ack, size_t len);                  // len >= 7 && ack[6] == CAP_BYTE

// CTRL (txt_type 8): ts(4) | 0x20 | sub(1) | versie(1) | [mbx_pub(32) | token(8)]  -> 47 of 7 bytes
constexpr size_t CTRL_INFO_LEN   = 47;
constexpr size_t CTRL_REVOKE_LEN = 7;
struct MbxInfo { uint8_t sub; uint8_t version; uint8_t mbx_pub[32]; uint8_t token[8]; };
size_t buildCtrlPlain(uint8_t* out, uint32_t ts, const MbxInfo& info);
bool   parseCtrlPlain(const uint8_t* data, size_t len, uint32_t& ts, MbxInfo& out);
// CTRL-plaintext rond een body vanaf sub, zoals de outbox hem bewaart (max MAX_TEXT bytes)
size_t ctrlPlainFromBody(uint8_t* out, uint32_t ts, const uint8_t* body, size_t len);

// Registratie (ANON_REQ-plaintext): ts(4) | 0xFF | 'M' | versie(1) | owner(4) | token(8)  -> 19 bytes
size_t buildRegReq(uint8_t* out, uint32_t ts, const uint8_t owner[4], const uint8_t token[8]);
bool   parseRegReq(const uint8_t* data, size_t len, uint32_t& ts, uint8_t owner[4], uint8_t token[8]);
// Antwoord (RESPONSE-body na tag): status(1) | ttl_dagen(1) | quota(1) | tijd_M(4)
size_t buildRegResp(uint8_t* out, MbxCode st, uint8_t ttl_days, uint8_t quota, uint32_t time_m);
bool   parseRegResp(const uint8_t* body, size_t len, MbxCode& st, uint8_t& ttl_days, uint8_t& quota, uint32_t& time_m);

// REQ-bodies
size_t buildDeposit(uint8_t* out, const uint8_t owner[4], const uint8_t* inner, uint8_t inner_len);
bool   parseDeposit(const uint8_t* body, size_t len, uint8_t owner[4], const uint8_t*& inner, uint8_t& inner_len);
size_t buildFetch(uint8_t* out, uint8_t flags, uint32_t store_id, const Report* r, uint8_t n);
bool   parseFetch(const uint8_t* body, size_t len, uint8_t& flags, uint32_t& store_id, Report* r, uint8_t& n);
size_t buildStatus(uint8_t* out, const uint8_t (*hashes)[8], uint8_t n);
bool   parseStatus(const uint8_t* body, size_t len, uint8_t (*hashes)[8], uint8_t& n);
size_t buildQuery(uint8_t* out, const QueryItem* q, uint8_t n);
bool   parseQuery(const uint8_t* body, size_t len, QueryItem* q, uint8_t& n);

// RESPONSE-bodies (na tag)
size_t buildDepositResp(uint8_t* out, MbxCode st, const uint8_t pkt_hash[8], uint32_t ttl_s);   // seconden tot M hem laat verlopen
bool   parseDepositResp(const uint8_t* body, size_t len, MbxCode& st, uint8_t pkt_hash[8], uint32_t& ttl_s);
size_t buildFetchResp(uint8_t* out, uint8_t remaining, uint8_t reports_ok, const uint8_t* inner, uint8_t inner_len);
bool   parseFetchResp(const uint8_t* body, size_t len, uint8_t& remaining, uint8_t& reports_ok,
                      const uint8_t*& inner, uint8_t& inner_len);
size_t buildStatusResp(uint8_t* out, const StatusReply* r, uint8_t n);
bool   parseStatusResp(const uint8_t* body, size_t len, StatusReply* r, uint8_t& n);
size_t buildQueryResp(uint8_t* out, const QueryReply* r, uint8_t n);
bool   parseQueryResp(const uint8_t* body, size_t len, QueryReply* r, uint8_t& n);

}}
```

CTRL-plaintext wordt alleen in de codec opgebouwd: `Outbox` (ACK van een CTRL-entry) en `Node::sendDm` via `ctrlPlainFromBody`, `Node::onCtrlTxt` rekent het ACK over `buildCtrlPlain` van de geparste velden. Omdat `parseCtrlPlain` na `CTRL_INFO_LEN`/`CTRL_REVOKE_LEN` alleen nullen accepteert, zijn dat exact de ontvangen bytes zonder blokpadding.

Wire-samenvatting (bytes, zonder de 4-byte timestamp of tag):

| body | indeling | max |
|---|---|---|
| DEPOSIT | `0x41 | owner(4) | inner_len(1) | inner(≤164)` | 170 |
| FETCH | `0x42 | flags(1) | store_id(4) | n(1) | n × (hash(8) | result(1) | ack(6))` | 127 |
| STATUS | `0x44 | n(1) | n × hash(8)` | 66 |
| RECEIPT_QUERY | `0x45 | n(1) | n × (ts(4) | K(4))` | 66 |
| DEPOSIT-antwoord | `status(1) | hash(8) | ttl_s(4)` (relatief: geen klokafstemming tussen Bob en M nodig) | 13 |
| FETCH-antwoord | `resterend(1) | rapporten_ok(1) | inner_len(1) | inner` | 167 |
| STATUS-/QUERY-antwoord | `n(1) | n × (state(1) | ack(6))` | 57 |
| registratie-antwoord | `status(1) | ttl(1) | quota(1) | tijd_M(4)` | 7 |

### 3.5 `RdmStorage.h`

```cpp
namespace rdm {

class FileIO {
public:
  virtual ~FileIO() {}
  virtual bool     exists(const char* path) = 0;
  virtual int32_t  size(const char* path) = 0;                       // -1 als het bestand ontbreekt
  virtual bool     create(const char* path, uint32_t size) = 0;      // nieuw bestand, nullen, vervangt bestaand
  virtual bool     read(const char* path, uint32_t off, uint8_t* buf, uint32_t len) = 0;
  virtual bool     write(const char* path, uint32_t off, const uint8_t* buf, uint32_t len) = 0;  // in-place
  virtual bool     remove(const char* path) = 0;
  virtual uint32_t freeBytes() = 0;
};

// Bestandsformaat: header "RDMF" | ver(1) | ab(1) | payload_size(2) | slots(2) | reserved(6) = 16 bytes,
// daarna per kopie: seq(4) | crc16(2) | used(1) | pad(1) | payload. CRC-16/CCITT over seq|used|payload.
// ab = true: twee kopieën per slot; lezen neemt de geldige met de hoogste seq, schrijven overschrijft de andere.
class RecordFile {
public:
  enum class Open : uint8_t { OPENED, CREATED, MIGRATED, FAILED };
  typedef bool (*MigrateFn)(uint8_t from_ver, const uint8_t* in, uint16_t in_size, uint8_t* out);

  RecordFile(FileIO& io, const char* path, uint8_t format_ver, uint16_t payload_size, uint16_t slots, bool ab);
  Open     open(MigrateFn migrate = nullptr);   // andere slots/ab: MIGRATED (records blijven, overtollige slots vallen weg);
                                                // andere payload_size/versie zonder migrate: CREATED. Paden max 35 tekens.
  bool     read(uint16_t slot, uint8_t* payload) const;   // false: leeg of CRC fout
  bool     write(uint16_t slot, const uint8_t* payload);
  bool     erase(uint16_t slot);
  uint16_t slots() const;
  uint16_t payloadSize() const;
  static uint32_t fileSize(uint16_t payload_size, uint16_t slots, bool ab);
};

}
```

### 3.6 `RdmClock.h`

`/rdm/meta` is een `RecordFile` met één A/B-slot: `store_id(4) | rdm_time(4) | last_req_ts(4)`.

```cpp
namespace rdm {
class Clock {
public:
  static constexpr uint16_t    RECORD_SIZE = 12;
  static constexpr const char* PATH = "/rdm/meta";

  explicit Clock(RecordFile& meta);
  bool     begin(uint32_t millis_now, bool storage_recreated, uint32_t random32);  // nieuwe store_id bij recreate
  uint32_t now(uint32_t millis_now, uint32_t rtc_now, bool rtc_trusted);          // G6, monotoon
  void     maybePersist(uint32_t millis_now, bool force);                          // elke RDM_CLOCK_PERSIST_S of force
  uint32_t nextReqTimestamp(uint32_t rtc_now);                                     // max(rtc_now, laatste + 1), persistent (K2)
  uint32_t storeId() const;
  uint32_t nextPersistMillis() const;                                              // millis waarop maybePersist() schrijft
  // ms vanaf millis_now tot de RDM-tijd rdm_due bereikt, aannemend dat hij met millis meeloopt; mag te vroeg, nooit te laat
  uint32_t millisUntil(uint32_t rdm_due, uint32_t millis_now) const;
};
}
```

### 3.7 `RdmContacts.h`

```cpp
namespace rdm {
struct ContactRdm {                 // record-payload, 52 bytes
  uint8_t  pub_prefix[6];
  uint8_t  flags;                   // CR_CAP 0x01, CR_HAS_MBX 0x02, CR_MBX_INFO_SENT 0x04
  uint8_t  reserved;
  uint32_t watermark;               // hoogste verwijderde sender-ts (ontvangerkant, I7)
  uint8_t  mbx_pub[32];             // mailbox van dit contact (afzenderkant)
  uint8_t  token[8];                // token_B voor die mailbox
};
enum : uint8_t { CR_CAP = 0x01, CR_HAS_MBX = 0x02, CR_MBX_INFO_SENT = 0x04 };

class ContactTable {
public:
  static constexpr uint16_t    RECORD_SIZE = 52;           // ContactRdm, gepakt
  static constexpr const char* PATH = "/rdm/contacts";

  explicit ContactTable(RecordFile& file);
  bool        begin();
  ContactRdm* find(const uint8_t pub_prefix[6]);
  ContactRdm* findOrAdd(const uint8_t pub_prefix[6]);   // evict: oudste zonder CR_HAS_MBX en watermark 0
  bool        save(const ContactRdm& c);
  ContactRdm* findByMailbox(const uint8_t mbx_pub_prefix[6]);
  uint16_t    count() const;                           // slots, gebruikt of niet
  ContactRdm* at(uint16_t i);                          // nullptr voor een leeg slot
};
}
```

Contract: een pointer uit `find`/`findOrAdd`/`at` blijft geldig tot de volgende `findOrAdd`; aanpassen via de pointer en daarna `save(*c)` persisteert (WP4 zet bij een mislukte `save` de oude waarde terug).

### 3.8 `RdmOutbox.h`

```cpp
namespace rdm {

struct OutEntry {                   // record-payload, 201 bytes
  uint8_t  pub_prefix[6];
  uint32_t ts;
  uint8_t  key[4];
  uint8_t  klass;                   // attempt & 3 van de eerste app-poging
  uint8_t  fw_attempt;              // volgende firmware-attempt: 252 + klass, aflopend per 4, wrap naar 252 + klass
  OutState state;
  uint8_t  flags;                   // OF_UNREPORTED, OF_CONFIRM_PENDING, OF_CONFIRM_SENT, OF_MBX_COPY, OF_CTRL
  uint32_t app_ack;                 // ACK_R van de laatste app-poging (voor SEND_CONFIRMED)
  uint8_t  pkt_hash[8];             // mailboxkopie (OF_MBX_COPY)
  uint32_t created;
  uint32_t deadline;                // T_radio, T_sync of nu + ttl_s van M + grace, RDM-tijd
  uint32_t final_at;                // 0 tot eindstate
  uint8_t  text_len;
  char     text[MAX_TEXT + 1];      // bij OF_CTRL: CTRL-body vanaf sub
};
enum : uint8_t { OF_UNREPORTED = 0x01, OF_CONFIRM_PENDING = 0x02, OF_CONFIRM_SENT = 0x04, OF_MBX_COPY = 0x08, OF_CTRL = 0x10 };

class OutboxHost {
public:
  virtual ~OutboxHost() {}
  virtual void selfPub(uint8_t pub_out[32]) = 0;                                    // eigen sleutel: afzender in ACK_R/ACK_S/K/CTRL-ACK
  virtual uint32_t random32() = 0;                                                  // jitter (G16)
  virtual bool contactPub(const uint8_t pub_prefix[6], uint8_t pub_out[32]) = 0;   // false: contact weg
  virtual bool hasDirectPath(const uint8_t pub_prefix[6]) = 0;
  virtual bool txIdle() = 0;
  virtual bool sendDm(const OutEntry& e, uint8_t attempt, bool flood, uint32_t& est_timeout_ms) = 0;
  virtual bool sendReceiptQuery(const uint8_t pub_prefix[6], const QueryItem* q, uint8_t n, bool flood,
                                uint32_t& est_timeout_ms) = 0;
  virtual bool sendDeposit(const OutEntry& e, uint8_t attempt, uint8_t pkt_hash_out[8], uint32_t& est_timeout_ms) = 0;
  virtual bool sendStatus(const uint8_t pub_prefix[6], const uint8_t (*hashes)[8], uint8_t n, uint32_t& est_timeout_ms) = 0;
  virtual bool sendRegister(const uint8_t pub_prefix[6], uint32_t& est_timeout_ms) = 0;
  virtual bool pushUserStatus(const OutEntry& e, UserStatus s) = 0;   // true: 0x91-client heeft hem
  virtual bool pushSendConfirmed(const OutEntry& e) = 0;              // true: app verbonden
};

class Outbox {
public:
  static constexpr uint16_t    RECORD_SIZE = 201;          // OutEntry, gepakt
  static constexpr const char* PATH = "/rdm/outbox";
  static constexpr uint16_t    WATCH_RECORD_SIZE = 28;     // receipt-watch na SYNC_EXPIRED
  static constexpr const char* WATCH_PATH = "/rdm/watch";

  enum class SendResult : uint8_t { NEW_ENTRY, EXISTING_ENTRY, NO_OUTBOX, TOO_LONG };

  Outbox(RecordFile& file, RecordFile& watch, ContactTable& contacts, OutboxHost& host);
  bool       begin(uint32_t now);                          // laden, ACK's herberekenen, boot-kick plannen
  SendResult onAppSend(const uint8_t pub_prefix[6], uint32_t ts, uint8_t attempt, const char* text, size_t text_len,
                       uint32_t now, uint32_t& app_ack_out, bool& transmit_out);
  void       onAppTransmitted(const uint8_t pub_prefix[6], uint32_t ts, uint32_t est_timeout_ms, uint32_t now);
  bool       addCtrl(const uint8_t pub_prefix[6], const uint8_t* ctrl_plain, size_t len, uint32_t now);  // MBX_INFO/REVOKE
  bool       onAck(const uint8_t* ack, uint8_t len, uint32_t now);          // 4-7 bytes; true bij match
  void       onQueryReply(const uint8_t pub_prefix[6], const QueryItem* asked, const QueryReply* r, uint8_t n, uint32_t now);
  void       onDepositReply(MbxCode st, const uint8_t pkt_hash[8], uint32_t ttl_s, uint32_t now);   // matched on pkt_hash
  void       onStatusReply(const uint8_t pub_prefix[6], const uint8_t (*asked)[8], const StatusReply* r, uint8_t n, uint32_t now);
  void       onRegisterReply(const uint8_t pub_prefix[6], MbxCode st, uint32_t now);
  void       onHeard(const uint8_t pub_prefix[6], bool had_cap_trailer, uint32_t now);   // G15: cap wissen zonder trailer
  void       onMailboxAdvert(const uint8_t mbx_pub_prefix[6], uint32_t now);
  void       onMailboxChanged(const uint8_t pub_prefix[6], uint32_t now);  // nieuwe MBX_INFO of REVOKE (G14)
  void       onClientConnected(bool rdm_client, uint32_t now);             // G5
  void       loop(uint32_t now);
  uint32_t   nextDue(uint32_t now) const;   // vroegste RDM-tijd waarop loop() iets doet; nooit < now; UINT32_MAX: niets
  uint8_t    count() const;
  bool       get(uint8_t idx, OutEntry& out) const;
};

}
```

Tags en time-outs (besluit WP3): `Node` kiest de REQ-timestamp zelf (`Clock::nextReqTimestamp`) en houdt tag-naar-soort bij (RAM, max 16 openstaand), zodat hij elk antwoord naar de juiste module en methode stuurt. De Outbox matcht antwoorden op inhoud (DEPOSIT op `pkt_hash`, STATUS op contact en `pkt_hash`, query op contact en `(ts, K)`, registratie op contact) en bewaakt time-outs met eigen timers (G3, G9). M vult `pkt_hash` in elk DEPOSIT-antwoord in, ook bij een foutcode.

Receipt-watch (besluit WP3): tweede `RecordFile` `Outbox::WATCH_PATH` (`/rdm/watch`), payload `WATCH_RECORD_SIZE` = 28 bytes: `ack_s`(6) | verloop(4) | pub_prefix(6) | ts(4) | K(4) | app_ack(4). Een late `ACK_S` na een reboot wordt zo nog herkend.

### 3.9 `RdmInbox.h`

```cpp
namespace rdm {

struct InRecord {                   // inbox-payload, 188 bytes
  uint8_t  sender_prefix[6];
  uint32_t ts;
  uint32_t recv_time;
  uint8_t  txt_type;
  uint8_t  path_len;                // 0xFF = direct
  int8_t   snr_x4;
  uint8_t  flags;                   // IF_VIA_MAILBOX 0x01, IF_SENDER_CAP 0x02
  uint8_t  mbx_hash[8];
  uint8_t  text_len;
  char     text[INBOX_TEXT_MAX + 1];  // tot 160: ook lange DM's van standaard afzenders
};
struct RegRecord {                  // register-payload, 36 bytes
  uint8_t  sender_prefix[6];
  uint32_t ts;
  uint8_t  key[4];
  uint8_t  ack_r[4];
  uint8_t  ack_s[6];
  uint8_t  state;                   // 0 ON_RADIO, 1 SYNCED
  uint8_t  flags;                   // IF_VIA_MAILBOX, IF_SENDER_CAP, RF_REPORT_PENDING 0x04, RF_REPORT_DUP 0x08, RF_MBX_OUTCOME 0x10
  uint8_t  mbx_hash[8];
  uint16_t inbox_slot;              // 0xFFFF na sync
};
struct RecvInput {
  const uint8_t* sender_pub;        // 32
  uint32_t ts; uint8_t flags; uint8_t txt_type;
  const char* text; uint8_t text_len;
  bool sender_cap;
  bool via_mailbox; const uint8_t* mbx_hash;   // 8 bytes bij via_mailbox
  uint8_t path_len; int8_t snr_x4;
};
struct RecvDecision {
  RecvResult result;
  bool send_ack_r;                  // direct naar afzender (false bij mailboxkopie en bij INBOX_FULL/STORE_ERROR)
  bool send_ack_s;                  // duplicaat van al gesynct bericht, afzender met cap, niet via mailbox
  bool cap_byte;                    // ACK_R met CAP_BYTE
  uint8_t ack_r[4]; uint8_t ack_s[6];
};

class InboxHost {
public:
  virtual ~InboxHost() {}
  virtual void onInboxChanged() = 0;                                       // PUSH_CODE_MSG_WAITING
  virtual bool sendAckS(const uint8_t sender_prefix[6], const uint8_t ack_s[6]) = 0;
  virtual void onReportQueued() = 0;                                       // Fetcher: FETCH na sync
};

class Inbox {
public:
  static constexpr uint16_t    RECORD_SIZE = 188;          // InRecord, gepakt
  static constexpr const char* PATH = "/rdm/inbox";
  static constexpr uint16_t    REG_RECORD_SIZE = 36;       // RegRecord, gepakt
  static constexpr const char* REG_PATH = "/rdm/register";

  Inbox(RecordFile& inbox, RecordFile& reg, ContactTable& contacts, InboxHost& host);
  bool         begin(bool& recreated_out);
  RecvDecision onMessage(const RecvInput& in, uint32_t now);
  bool         nextForApp(InRecord& out, uint16_t& slot_out);   // oudste ongesynct, nog niet aangeboden op deze verbinding
  void         onHandedToApp(uint16_t slot);
  void         onNextSyncRequest(uint32_t now);                  // bevestigt de laatst aangeboden slot: SYNCED
  void         onClientDisconnected();
  QueryReply   query(const uint8_t sender_prefix[6], const QueryItem& q);
  uint8_t      pendingReports(Report* out, uint8_t max);
  void         onReportsConfirmed(const Report* sent, uint8_t n_ok);
  void         onUndecryptable(const uint8_t mbx_hash[8]);       // rapport UNDECRYPTABLE voor deze mailboxkopie
  uint16_t     freeSlots() const;
};

}
```

`RF_REPORT_DUP`: het openstaande rapport is DUPLICATE in plaats van ON_RADIO; `RF_MBX_OUTCOME`: `mbx_hash` hoort bij een mailboxkopie die nog een uitkomst moet krijgen. `Inbox::begin` opent inbox en register zelf; algemeen geldt: elke module opent zijn eigen `RecordFile` in `begin`, `Node` construeert alleen.

Inbox-regels uit WP4 (verwerkt in `03`): kopie van M voor een al gesynct bericht geeft rapport SYNCED; een kopie die niet past (quotum, vol, schrijffout) geeft INBOX_FULL zonder directe nieuwe FETCH; nieuwe ON_RADIO-, DUPLICATE- en SYNCED-rapporten roepen `onReportQueued`; rapporten zonder eigen registerregel staan alleen in RAM (max `MAX_BATCH`); herstel bij boot wist ongesyncte registerregels zonder inboxrecord en afgebroken syncs, en biedt inboxrecords zonder regel aan (D2); een inboxrecord met CRC-fout gaat samen met zijn regel weg; wordt het register nieuw aangemaakt, dan wist `begin` alle watermerken (via `ContactTable::count/at`).

### 3.10 `RdmFetcher.h`

```cpp
namespace rdm {
class FetcherHost {
public:
  virtual ~FetcherHost() {}
  virtual bool ownMailbox(uint8_t mbx_pub_out[32]) = 0;           // false: geen mailbox ingesteld
  virtual bool sendFetch(uint8_t flags, uint32_t store_id, const Report* r, uint8_t n, uint32_t& est_timeout_ms) = 0;
  virtual uint32_t random32() = 0;                                  // jitter (G17)
  virtual bool canFetch() { return true; }                          // false: sendFetch zou nu weigeren
};
class Fetcher {
public:
  Fetcher(Inbox& inbox, FetcherHost& host);
  void begin(uint32_t now, uint32_t store_id, uint32_t random32);   // boot-FETCH na 0-60 s jitter
  void onFetchReply(uint8_t remaining, uint8_t reports_ok, bool had_payload, uint32_t now);
  void onFetchTimeout(uint32_t now);
  void onMailboxAdvert(uint32_t now);
  void onReportQueued(uint32_t now);
  void loop(uint32_t now);
  uint32_t nextDue(uint32_t now) const;     // volgende (boot-)FETCH of fetch-time-out; UINT32_MAX: geen eigen mailbox
};
}
```

`canFetch()` laat de Fetcher een FETCH overslaan die de host toch zou weigeren, zonder eerst de openstaande rapporten uit het register te lezen; `Node` geeft `ownMailbox && reqAllowed` (REQ-gap per mailbox). `pendingReports` en `freeSlots` lezen alleen, dus het overslaan verandert niets aan het gedrag: de FETCH blijft due en gaat mee in de eerstvolgende `loop` waarin hij mag.

Fetcher-regels uit WP4: minimaal `RDM_MBX_REQ_GAP_S` tussen twee FETCH's; één FETCH tegelijk, eigen time-out `max(2 × est_timeout, RDM_WAIT_ACK_MIN_S)` zonder directe herhaling; een laat antwoord bevestigt de rapporten alsnog; doorloop bij `resterend > 0` behalve bij volle inbox of openstaand INBOX_FULL; advert-trigger minimaal 10 min na de laatste FETCH van welke soort ook.

### 3.11 `RdmNode.h`

```cpp
namespace rdm {

// Mesh-kant van de node: tijd, toeval, contacten en zenden (de adapter van RdmChatMesh).
class NodeMeshHost {
public:
  virtual ~NodeMeshHost() {}
  virtual uint32_t millis() = 0;
  virtual uint32_t random32() = 0;
  virtual bool     txIdle() = 0;
  virtual void     selfPub(uint8_t pub_out[32]) = 0;       // eigen identiteit (self_id.pub_key)
  virtual bool lookupContact(const uint8_t pub_prefix[6], uint8_t pub_out[32], bool& favourite, bool& has_path) = 0;
  virtual bool sendTxtPlain(const uint8_t pub_prefix[6], const uint8_t* plain, size_t len, bool flood, uint32_t& est_timeout_ms) = 0;
  virtual bool sendAck(const uint8_t pub_prefix[6], const uint8_t* ack, uint8_t len) = 0;
  virtual bool sendReq(const uint8_t peer_pub[32], uint32_t req_ts, const uint8_t* body, size_t len, bool flood,
                       uint32_t& est_timeout_ms) = 0;
  virtual bool sendAnonReq(const uint8_t peer_pub[32], const uint8_t* plain, size_t len, uint32_t& est_timeout_ms) = 0;
  virtual bool encryptTxtPayload(const uint8_t pub_prefix[6], const uint8_t* plain, size_t len,
                                 uint8_t* payload_out, size_t& payload_len) = 0;
  virtual bool decryptTxtPayload(const uint8_t* payload, size_t len, uint8_t sender_pub_out[32],
                                 uint8_t* plain_out, size_t& plain_len) = 0;
  // K3: de set verborgen peers (eigen mailbox, mailboxen van contacten) is gewijzigd, ook één keer na begin()
  virtual void onHiddenPeersChanged() {}
};

// App-kant van de node: de companion (MyMesh) of de simulator.
class NodeAppSink {
public:
  virtual ~NodeAppSink() {}
  virtual uint32_t rtcNow(bool& trusted) = 0;
  virtual bool pushUserStatus(const uint8_t pub_prefix[6], uint32_t app_ack, UserStatus s, const uint8_t key[4], uint32_t ts) = 0;
  virtual bool pushSendConfirmed(uint32_t app_ack) = 0;
  virtual void pushMsgWaiting() = 0;
};

class Node : private OutboxHost, private InboxHost, private FetcherHost {
public:
  static constexpr uint16_t    MBX_RECORD_SIZE = 32 + 16;   // eigen mailbox: pubkey | K_owner
  static constexpr const char* MBX_PATH = "/rdm/mbx";

  Node(FileIO& io, NodeMeshHost& mesh, NodeAppSink& app);
  bool begin();                      // bestanden openen, aantallen uit io.freeBytes(); false = RDM uit
  bool enabled() const;
  void setEnabled(bool on);          // runtime-uit: exact upstreamgedrag (scenario 10, standaard-Alice in de sim)
  void loop();
  // ms tot loop() werk heeft: min over Outbox/Fetcher::nextDue (via Clock::millisUntil) en Clock::nextPersistMillis;
  // 0 = nu, UINT32_MAX = niets; mag te vroeg, nooit te laat. Voor fast-forward in de sim en slaapbeslissingen op het device.
  uint32_t nextWakeupMillis(uint32_t millis_now) const;

  // afzender: CMD_SEND_TXT_MSG. handled = false: geen outbox-bericht, de app-kant zendt het op de upstream-manier.
  // sent = false met handled: de entry blijft in de outbox, die hem zendt (CUSTODY, ON_RADIO, radio bezet).
  struct AppSend { bool handled; bool sent; uint32_t app_ack; uint32_t est_timeout_ms; };
  AppSend appSend(const uint8_t pub_prefix[6], uint32_t ts, uint8_t attempt, const char* text, size_t text_len);

  // ontvangen (aangeroepen door RdmChatMesh)
  RecvDecision onPlainTxt(const uint8_t sender_pub[32], const uint8_t* data, size_t len, uint8_t path_len, int8_t snr_x4);
  void    onCtrlTxt(const uint8_t sender_pub[32], const uint8_t* data, size_t len);
  bool    onAck(const uint8_t* ack, uint8_t len);
  uint8_t onReceiptQuery(const uint8_t sender_pub[32], const uint8_t* body, size_t len, uint8_t* reply_body);
  bool    onResponse(const uint8_t peer_pub[32], const uint8_t* data, size_t len);   // true: tag was van RDM
  void    onHeard(const uint8_t pub_prefix[6], bool had_cap_trailer);
  void    onAdvert(const uint8_t pub[32]);

  // verborgen peers (mailboxen) voor RdmChatMesh::searchPeersByHash
  uint8_t        hiddenPeerCount() const;
  const uint8_t* hiddenPeerPub(uint8_t i) const;

  // app-kant
  bool    nextInboxFrame(InRecord& out, uint16_t& slot);
  void    onInboxFrameHanded(uint16_t slot);
  void    onSyncRequest();
  void    onClientConnected(bool rdm_client);
  void    onClientDisconnected();
  bool    setOwnMailbox(const uint8_t* mbx_pub, const uint8_t* k_owner);   // nullptr, nullptr = geen
  uint8_t listOutbox(OutEntry* out, uint8_t max, uint8_t offset = 0);   // vanaf entry offset (streamen zonder grote buffer)

private:
  FileIO&       _io;
  NodeMeshHost& _mesh;
  NodeAppSink&  _app;

  // OutboxHost
  void selfPub(uint8_t pub_out[32]) override;
  uint32_t random32() override;
  bool contactPub(const uint8_t pub_prefix[6], uint8_t pub_out[32]) override;
  bool hasDirectPath(const uint8_t pub_prefix[6]) override;
  bool txIdle() override;
  bool sendDm(const OutEntry& e, uint8_t attempt, bool flood, uint32_t& est_timeout_ms) override;
  bool sendReceiptQuery(const uint8_t pub_prefix[6], const QueryItem* q, uint8_t n, bool flood,
                        uint32_t& est_timeout_ms) override;
  bool sendDeposit(const OutEntry& e, uint8_t attempt, uint8_t pkt_hash_out[8], uint32_t& est_timeout_ms) override;
  bool sendStatus(const uint8_t pub_prefix[6], const uint8_t (*hashes)[8], uint8_t n, uint32_t& est_timeout_ms) override;
  bool sendRegister(const uint8_t pub_prefix[6], uint32_t& est_timeout_ms) override;
  bool pushUserStatus(const OutEntry& e, UserStatus s) override;
  bool pushSendConfirmed(const OutEntry& e) override;
  // InboxHost
  void onInboxChanged() override;
  bool sendAckS(const uint8_t sender_prefix[6], const uint8_t ack_s[6]) override;
  void onReportQueued() override;
  // FetcherHost
  bool ownMailbox(uint8_t mbx_pub_out[32]) override;
  bool canFetch() override;
  bool sendFetch(uint8_t flags, uint32_t store_id, const Report* r, uint8_t n, uint32_t& est_timeout_ms) override;
};

}
```

`NodeAppSink::rtcNow(trusted)`: `trusted` is alleen waar als de app sinds boot de tijd zette (`CMD_SET_DEVICE_TIME`) of er een hardware-RTC is. De klok die de companion bij boot uit de contacten afleidt (`bootstrapRTCfromContacts`, `examples/companion_radio/MyMesh.cpp:1000`) is niet betrouwbaar (`03` par. 11, G6).

Slotaantallen per board (besluit WP6): T-Beam (ESP32, 4 MB) bouwt met `RDM_OUTBOX_SLOTS_MAX=8`, `RDM_WATCH_SLOTS_MAX=64`, `RDM_REGISTER_SLOTS_MAX=256`, `RDM_INBOX_SLOTS_MAX=128`, `RDM_CONTACT_SLOTS_MAX=32` (anders past DRAM niet; `Node` ~12 KB RAM); RAK met `RDM_WATCH_SLOTS_MAX=32`.

De host-overrides staan in de header omdat `RdmChatMesh` een `rdm::Node` als member heeft; zonder deze declaraties is `Node` abstract. `test_rdm_headers` controleert dat met `std::is_abstract`, en met `std::is_base_of` dat `RdmChatMesh` zelf geen host-interface is (AR3).

`Node::appSend` is het companion-recept voor `CMD_SEND_TXT_MSG` (AR1/D29): de outbox beslist (`Outbox::onAppSend`), de plaintext krijgt de cap-trailer, gaat via `sendTxtPlain(flood = false)` en bij succes volgt de boekhouding van `Outbox::onAppTransmitted`. `handled == false` (K6, TOO_BIG, RDM uit): de app-kant zendt het bericht op de upstream-manier; bij K6 en TOO_BIG heeft `Node` dan al de status gepusht.

### 3.12 `mesh/RdmChatMesh.h` (integratielaag)

```cpp
#include <helpers/BaseChatMesh.h>
#include <helpers/rdm/RdmCodec.h>
#include <helpers/rdm/RdmNode.h>

class RdmChatMesh : public BaseChatMesh {
protected:
  RdmChatMesh(mesh::Radio& radio, mesh::MillisecondClock& ms, mesh::RNG& rng, mesh::RTCClock& rtc,
              mesh::PacketManager& mgr, mesh::MeshTables& tables, rdm::FileIO& io, rdm::NodeAppSink& app);
  rdm::Node& rdm();

  // CMD_SEND_TXT_MSG: de outbox beslist, de DM gaat met cap-trailer. handled = false: geen outbox-bericht,
  // zend het op de upstream-manier. De app krijgt RESP_CODE_SENT met app_ack en est_timeout_ms.
  struct AppSend { bool handled; bool flood; uint32_t app_ack; uint32_t est_timeout_ms; };
  AppSend rdmSendApp(const ContactInfo& to, uint32_t ts, uint8_t attempt, const char* text, size_t len);
  // CMD_SYNC_NEXT_MESSAGE: elk verzoek bevestigt het record dat ervoor is overhandigd (03 par. 1c); false: inbox leeg
  bool    rdmNextInbox(rdm::InRecord& out);

  // Mesh/BaseChatMesh-overrides
  int  searchPeersByHash(const uint8_t* hash) override;            // basis + verborgen peers achteraan
  void getPeerSharedSecret(uint8_t* dest_secret, int peer_idx) override;
  void onPeerDataRecv(mesh::Packet* pkt, uint8_t type, int sender_idx, const uint8_t* secret, uint8_t* data, size_t len) override;
  bool onPeerPathRecv(mesh::Packet* pkt, int sender_idx, const uint8_t* secret, uint8_t* path, uint8_t path_len,
                      uint8_t extra_type, uint8_t* extra, uint8_t extra_len) override;
  bool onContactPathRecv(ContactInfo& from, uint8_t* in_path, uint8_t in_path_len, uint8_t* out_path, uint8_t out_path_len,
                         uint8_t extra_type, uint8_t* extra, uint8_t extra_len) override;
  void onAckRecv(mesh::Packet* pkt, uint32_t ack_crc) override;
  void onAdvertRecv(mesh::Packet* pkt, const mesh::Identity& id, uint32_t ts, const uint8_t* app_data, size_t len) override;
  // K3: contact verwijderd omdat het een verborgen peer is; de app-kant meldt het aan de app en persisteert de contacten
  virtual void rdmOnContactRemoved(const ContactInfo& removed) {}
  // Een plain DM is in de inbox opgeslagen (RecvResult::NEW): de UI-preview die upstream in onMessageRecv geeft (D60)
  virtual void rdmOnMessageStored(mesh::Packet* pkt, ContactInfo& from, const rdm::codec::TxtParsed& p) {}

private:
  // Wat rdm::Node van de mesh ziet; zet door naar privé-methoden van RdmChatMesh. RdmChatMesh is zelf geen host:
  // BaseChatMesh::sendAnonReq blijft zichtbaar voor de afgeleide klasse (AR3)
  class MeshHost : public rdm::NodeMeshHost {
    RdmChatMesh& _m;
  public:
    explicit MeshHost(RdmChatMesh& m) : _m(m) {}
  };

  MeshHost  _host;
  rdm::Node _rdm;
};
```

`MyMesh` erft onder `WITH_RELIABLE_DM` van `RdmChatMesh` in plaats van `BaseChatMesh` en implementeert `rdm::NodeAppSink` privé (`rtcNow`, `pushUserStatus`, `pushSendConfirmed`, `pushMsgWaiting`); de constructor geeft `*this` als app-kant mee. De simulator-companion doet hetzelfde.

`rdmSendApp` roept `Node::appSend` aan; blijft de DM in de outbox (`sent == false`), dan rekent `rdmSendApp` de `est_timeout_ms` uit die de app anders had gekregen (airtime van de datagramlengte die `Mesh::createDatagram` zou bouwen, via `calcFloodTimeoutMillisFor`/`calcDirectTimeoutMillisFor`), naast `sendToPeer` (R4). `flood` = `to.out_path_len == OUT_PATH_UNKNOWN`. `rdmNextInbox` doet `onSyncRequest`, `nextInboxFrame` en `onInboxFrameHanded` in één aanroep; omdat de companion synchroon antwoordt, is dat gelijk aan de eerdere volgorde met `onInboxFrameHanded` na het schrijven van het frame.

### 3.13 Wijzigingen in upstream-bestanden (onder flag)

```cpp
// src/helpers/BaseChatMesh.h, sectie protected:
#ifdef WITH_RELIABLE_DM
  ContactInfo* rdmMatchedPeer(int sender_idx);                                   // contacts[matching_peer_indexes[i]]
  void rdmSendAckTo(const ContactInfo& dest, const uint8_t* ack, uint8_t len);    // roept sendAckTo aan
#endif
```

```cpp
// src/Mesh.cpp, Mesh::createDatagram
#if defined(WITH_RELIABLE_DM) || defined(WITH_DM_MAILBOX)
    if (PATH_HASH_SIZE * 2 + CIPHER_MAC_SIZE + ((data_len + CIPHER_BLOCK_SIZE - 1) / CIPHER_BLOCK_SIZE) * CIPHER_BLOCK_SIZE
        > MAX_PACKET_PAYLOAD) return NULL;
#else
    if (data_len + CIPHER_MAC_SIZE + CIPHER_BLOCK_SIZE-1 > MAX_PACKET_PAYLOAD) return NULL;
#endif
```

`RdmChatMesh.h` houdt de mesh-kant van `rdm::NodeMeshHost` privé, achter de geneste adapter `MeshHost` (AR3; eerder een protected overerving, besluit WP5). `RdmChatMesh` handelt `RECEIPT_QUERY` (0x45) zelf af in `onPeerDataRecv`, zodat companion en simulator dezelfde code delen; met RDM uit valt de REQ door naar `onContactRequest` (geen antwoord, zoals standaard). `sendTxtPlain`/`sendReq`: `flood = true` dwingt flood af, anders direct bij een bekend pad (contact of verborgen peer), anders flood. Bekende beperking v1: geen `handleReturnPathRetry` voor RDM-ACK's (`Node::onAck` meldt het contact niet), dus padherstel na een verloren reciprocal PATH gaat trager.

K3 (besluit K3): `RdmChatMesh::onAdvertRecv` geeft een advert van een verborgen peer alleen aan `rdm().onAdvert`, niet aan `BaseChatMesh::onAdvertRecv`. `onHiddenPeersChanged()` (door `Node` aangeroepen na `begin()` en bij elke wijziging van de verborgen peers) verwijdert contacten met de pubkey van een verborgen peer via `removeContact` en roept per contact `rdmOnContactRemoved`; `MyMesh` pusht dan `PUSH_CODE_CONTACT_DELETED` en slaat de contacten op.

**Recept voor de companion**: het recept staat in `Node::appSend`, `RdmChatMesh::rdmSendApp` en `RdmChatMesh::rdmNextInbox` (AR1/D29); `MyMesh`, `RdmSimCompanion` en `test/test_rdm_node/wire.h` roepen die aan in plaats van het na te bouwen. Wat de app-kant zelf doet:
- `MyMesh` erft onder flag van `RdmChatMesh` en krijgt een extra constructorparameter `rdm::FileIO&`. `main.cpp` maakt een `ArduinoFileIO` op het bestandssysteem van de `DataStore` en geeft die mee. Zodra die constructor verandert, past dm-current `CompanionNode::createMesh` aan (H5: `SimFileIO` op `fs()`).
- Volgorde bij opstarten: `store.begin()`, `begin()` van de mesh, contacten laden, daarna `rdm().begin()`; `rdm().loop()` in `loop()`.
- `CMD_SEND_TXT_MSG`: `rdmSendApp`; bij `handled` alleen `RESP_CODE_SENT` met `app_ack`, `flood` en `est_timeout_ms`. `handled == false`: het upstream-pad.
- `CMD_SYNC_NEXT_MESSAGE`: `rdmNextInbox` tot `false`, en pas als de inbox leeg is de gewone offline queue.
- UI-preview van een opgeslagen DM: `rdmOnMessageStored` (D60); `MyMesh` doet daar `markConnectionActive` en `Listener::onMessageRecv`.
- `rtcNow(trusted)`: `trusted` alleen na `CMD_SET_DEVICE_TIME` sinds boot of met een hardware-RTC, niet na `bootstrapRTCfromContacts` (vlag in `MyMesh`; de simulator leest hem via H1).
- Verbinding: `onClientConnected(rdm_client)` na `CMD_RDM_ENABLE` (0x91 alleen dan), `onClientDisconnected` bij verbreken.
- App-pushes: `pushUserStatus` -> 0x91 (20 bytes, 3.14), `pushSendConfirmed` -> `PUSH_CODE_SEND_CONFIRMED`, `pushMsgWaiting` -> `PUSH_CODE_MSG_WAITING`.
- Builds: controleer `<new>` (placement new in `RdmNode.h`) op ESP32 en nRF52; nRF52-env met `-D RDM_WATCH_SLOTS_MAX=32`.

### 3.14 Companion-frames

| frame | bytes |
|---|---|
| `CMD_RDM_ENABLE` | `0xC0 | versie(1)=1` -> `RESP_CODE_OK`, daarna 0x91 per `OF_UNREPORTED`-entry |
| `CMD_RDM_SET_MAILBOX` | `0xC1 | mbx_pub(32) | k_owner(16)` of alleen `0xC1` (geen mailbox) -> `RESP_CODE_OK` / `ERR_CODE_*` |
| `CMD_RDM_LIST_OUTBOX` | `0xC2` -> `RESP_CODE_RDM_OUTBOX_START`, per entry `RESP_CODE_RDM_OUTBOX_ENTRY`, afsluitend `RESP_CODE_RDM_OUTBOX_END` (patroon van `CMD_GET_CONTACTS`) |
| `RESP_CODE_RDM_OUTBOX_START` | `0x40 | count(1)` |
| `RESP_CODE_RDM_OUTBOX_ENTRY` | `0x41 | idx(1) | pub_prefix(6) | ts(4) | app_ack(4) | UserStatus(1) | OutState(1) | K(4)` = 22 bytes |
| `RESP_CODE_RDM_OUTBOX_END` | `0x42` |
| `PUSH_CODE_RDM_STATUS` | `0x91 | app_ack(4) | UserStatus(1) | K(4) | ts(4) | pub_prefix(6)` = 20 bytes |

Antwoordcodes liggen onder 0x80 en pushcodes vanaf 0x80, zoals in het companion protocol; clients scheiden daarop antwoorden van pushes (besluit WP6: de eerdere `0xC0`-antwoordcode lag in de pushreeks). Met RDM uit geven de drie `CMD_RDM_*` `ERR_CODE_BAD_STATE`. Codering in `examples/companion_radio/RdmCompanionProto.h` (header-only: een losse `.cpp` zou een object toevoegen aan stock builds, die `examples/companion_radio/*.cpp` bouwen; pure functies, unit-getest). `MyMesh` streamt de lijst met `Node::listOutbox(&e, 1, i)`, zonder buffer voor de hele outbox.

### 3.15 Mailbox: `mailbox/MailboxCore.h` en backend

```cpp
namespace rdm {

struct BackendReply {
  enum class Kind : uint8_t { STORE, REG, FETCH, STAT } kind;
  uint32_t id;
  MbxCode  code;
  uint32_t expires;                                  // STORE: absolute Pi-tijd; MailboxCore stuurt expires - backendTime() als ttl_s
  uint8_t  ttl_days, quota;                          // REG
  uint8_t  remaining, reports_ok, inner_len;         // FETCH
  uint8_t  inner[MAX_INNER_PAYLOAD];
  uint8_t  n; StatusReply items[MAX_BATCH];          // STAT
};

class MailboxBackend {
public:
  virtual ~MailboxBackend() {}
  virtual bool store(uint32_t id, const uint8_t owner[4], const uint8_t sender_pub[32], const uint8_t pkt_hash[8],
                     const uint8_t* payload, uint8_t len) = 0;
  virtual bool reg(uint32_t id, const uint8_t sender_pub[32], const uint8_t owner[4], const uint8_t token[8]) = 0;
  virtual bool fetch(uint32_t id, const uint8_t client_pub[32], uint8_t flags, uint32_t store_id, const Report* r, uint8_t n) = 0;
  virtual bool stat(uint32_t id, const uint8_t sender_pub[32], const uint8_t (*hashes)[8], uint8_t n) = 0;
  virtual bool poll(BackendReply& out) = 0;                                    // niet-blokkerend
  virtual bool pollAcl(uint8_t pub_out[32], bool& is_owner, uint8_t owner_out[4]) = 0;   // cache vullen na boot
  virtual bool ready() = 0;                                                    // mbx.ready met de eigen protocolversie ontvangen
  virtual uint32_t backendTime() = 0;                                          // Pi-unixtijd nu (laatste mbx.time + verstreken); 0 = onbekend
};

class MailboxCoreHost {
public:
  virtual ~MailboxCoreHost() {}
  virtual uint32_t millis() = 0;
  virtual bool txIdle() = 0;
  virtual bool addPeer(const uint8_t pub[32]) = 0;            // peer-cache voor ontsleutelen, LRU RDM_MBX_CLIENTS
  virtual bool sendResponse(const uint8_t client_pub[32], uint32_t tag, const uint8_t* body, size_t len, bool path_return) = 0;
};

class MailboxCore {
public:
  MailboxCore(MailboxBackend& be, MailboxCoreHost& host);
  void begin();
  void onRequest(const uint8_t client_pub[32], const uint8_t* data, size_t len, bool via_flood);   // data = ts(4) | body
  void onAnonRequest(const uint8_t client_pub[32], const uint8_t* plain, size_t len, bool via_flood);   // registratie
  void loop();                                                  // backend-antwoorden, 3 s time-out = NO_STORAGE, limieten
  uint32_t nextWakeupMillis(uint32_t millis_now) const;         // backend-time-out of gepaced antwoord; UINT32_MAX: niets
};

}
```

Regels in `MailboxCore` (aangevuld na WP7): een REQ of registratie via flood krijgt een PATH-return, anders gaat het antwoord direct (`via_flood`); rate-limited FETCH/STATUS en ANON boven de globale limiet krijgen geen antwoord, DEPOSIT/REG wel (RATE_LIMITED); backend niet ready: DEPOSIT/REG direct NO_STORAGE, FETCH/STATUS niets; maximaal 4 antwoorden in de wachtrij, één per loop bij een lege zendwachtrij, 300 ms vertraging; peer-cache LRU met `RDM_MBX_CLIENTS` (64) plaatsen en onbevestigde ANON-afzenders eerst verdrongen; `MailboxCore` houdt rate-limits bij voor evenveel clients (`CLIENTS`) en `MailboxMesh.h` eist met een `static_assert` dat `MBX_PEER_CACHE_SIZE` gelijk is. `MailboxMesh` forwardt niets, adverteert als `ADV_TYPE_ROOM` 30 s na boot en elke 12 u (flood) en decodeert base64 strikt. Een FETCH via flood gaat met `FETCH_FLAG_NO_PAYLOAD` naar de backend, zodat geen kopie op SENT komt die nooit verzonden wordt (WP8); `tijd_M` in het registratieantwoord is `backendTime()`; RATE_LIMITED komt alleen uit `MailboxCore`, nooit uit de daemon; replaycheck `ts < last_ts` per client (RAM); 1 REQ per `RDM_MBX_SERVER_REQ_GAP_S` (5 s) en 60 per uur per client; ANON_REQ globaal 4 per minuut; FETCH via flood krijgt een PATH-return met `resterend` maar zonder kopie; DEPOSIT groter dan 164 bytes inner geeft TOO_BIG zonder backend-aanroep.

### 3.16 Regelprotocol radio <-> `meshcore-mailboxd` (sessieprotocol v2)

115200 baud, regels eindigen op `\n`, velden gescheiden door één spatie, hex in kleine letters, base64 met padding. Regels die niet met `@MBX ` beginnen (debug-output van de radio) negeert de daemon; de radio leest alleen regels die met `mbx.` beginnen, in een buffer van 400 bytes. `PROTO` = 2 is de versie van dit sessieprotocol; beide kanten noemen hun versie bij het opzetten van de sessie.

| richting | regel |
|---|---|
| Pi -> radio | `mbx.hello?` ongevraagd bij de start van de daemon; de radio antwoordt altijd met `@MBX HELLO`, ook als hij al ready is |
| radio -> Pi | `@MBX HELLO <proto> <fw_versie>` bij boot, als antwoord op `mbx.hello?` en elke 30 s zolang de radio op `mbx.ready` wacht; `proto` = 2, decimaal; `fw_versie` is informatief (log van de daemon) en mag ontbreken |
| radio -> Pi | `@MBX STORE <id> <owner4hex> <sender32hex> <hash8hex> <payload_b64>` |
| radio -> Pi | `@MBX REG <id> <sender32hex> <owner4hex> <token8hex>` |
| radio -> Pi | `@MBX FETCH <id> <client32hex> <flags2hex> <store_id8hex> <rapporten>`, rapporten = `-` of `<hash8hex>:<result>:<ack12hex>` gescheiden door komma's |
| radio -> Pi | `@MBX STAT <id> <sender32hex> <hash8hex>[,<hash8hex>...]` |
| Pi -> radio | `mbx.acl <pub32hex> <o|d> <owner4hex>` na elke HELLO en na elke ACL-wijziging (eigenaren eerst, max 64), gevolgd door `mbx.ready <proto>` |
| Pi -> radio | `mbx.ready <proto>` sluit een ACL-reeks af; `proto` = 2. Zonder veld telt de regel als protocol 1 (de vorm van v1) |
| Pi -> radio | `mbx.time <unix>` na de `mbx.ready` die op een HELLO volgt en daarna elke 600 s; de radio gebruikt die voor `ttl_s` en `tijd_M` |
| Pi -> radio | `mbx.store <id> <code2hex> <expires>` pas na `COMMIT` met `synchronous=FULL`; `expires` is absolute Pi-tijd |
| Pi -> radio | `mbx.reg <id> <code2hex> <ttl_dagen> <quota>` |
| Pi -> radio | `mbx.fetch <id> <code2hex> <resterend> <rapporten_ok> <payload_b64 of ->` |
| Pi -> radio | `mbx.stat <id> <code2hex> <state>:<ack12hex>[,...]` |
| radio -> Pi (alleen test) | `@MBX TIME <unix>` zet de klok van `meshcore-mailboxd serve --stdio --fake-clock`; zonder die vlag genegeerd |

Sessie-opbouw. Beide kanten kunnen beginnen: de radio stuurt `@MBX HELLO` bij boot, de daemon stuurt `mbx.hello?` bij zijn start. Op elke HELLO met `proto` = 2 antwoordt de daemon met de volledige reeks: de ACL, `mbx.ready 2` en `mbx.time`. Een dubbele HELLO (de radio zag bijvoorbeeld zowel zijn boot als een `mbx.hello?`) krijgt de reeks nog eens; dat is onschadelijk. Zolang de radio nog geen `mbx.ready` heeft ontvangen, herhaalt hij HELLO elke 30 s als vangnet voor een verloren regel. Een `mbx.hello?` verandert de toestand van de radio niet: is hij ready, dan blijft hij dat en bevestigt de `mbx.ready 2` die volgt dat opnieuw. Of de radio reset wanneer de daemon de poort opent, doet er niet toe: beide startvolgorden leiden tot een sessie. Verzoeken (STORE, REG, FETCH, STAT) verwerkt de daemon onafhankelijk van de sessietoestand; de radio stuurt ze pas als hij ready is (par. 3.15).

ACL-wijziging. `owner-add`, `deny` en `undeny` draaien als CLI in een ander proces. De draaiende daemon stuurt daarna opnieuw de ACL-reeks plus `mbx.ready 2`, zonder `mbx.time`. Hoe hij de wijziging opmerkt, is aan de daemon. De radio accepteert een ACL-reeks altijd en idempotent: een peer die al in de cache zit, komt er niet nog eens in. Een peer die uit de ACL verdween (`deny`) blijft in de cache tot hij verdrongen wordt; de cache dient alleen voor ontsleutelen en routeren, de daemon weigert een geweigerde afzender zelf met NOT_AUTH. Een herstart van de daemon na `owner-add` is niet meer nodig.

Versie. De radio wordt ready op `mbx.ready 2` en alleen daarop. Op `mbx.ready` met een andere versie, of zonder versie (protocol 1), wordt hij niet ready, stopt hij de herhaling van HELLO, onthoudt hij de versie van de daemon (de display van de mailbox toont `Pi: proto <n>`) en wacht hij op een volgende `mbx.hello?`, bijvoorbeeld na een upgrade van de daemon. Niet ready betekent zoals in par. 3.15: DEPOSIT en REG krijgen NO_STORAGE, FETCH en STATUS geen antwoord. De daemon die een HELLO met een andere versie ontvangt, of de v1-vorm `@MBX HELLO [<fw_versie>]` zonder versieveld (eerste veld geen decimaal getal), logt één regel en antwoordt alleen met `mbx.ready 2`, zonder ACL en zonder `mbx.time`; een latere HELLO met `proto` = 2 krijgt gewoon de volledige reeks. Een `mbx.ready` met meer dan één veld of een niet-numeriek veld negeert de radio als ongeldige regel; `mbx.hello?` matcht alleen exact.

`id` is een 32-bit teller van de radio (decimaal). De daemon verwerkt regels strikt op volgorde. Toestanden, TTL, quotum, token, denylist en RESYNC volgen `03` par. 4 en 11; de conformance-vectoren en `examples/mailbox_server/meshcore-mailboxd/README.md` leggen de details vast en zijn daarin normatief (WP8): quotum telt STORED, SENT en ON_RADIO; controlevolgorde TOO_BIG, UNKNOWN_OWNER, NOT_AUTH, ALREADY_STORED, QUOTA; ALREADY_STORED alleen bij dezelfde afzender en owner; elk rapport telt in `rapporten_ok`; STAT toont alleen eigen berichten; foutieve regels krijgen geen antwoord (de radio valt na 3 s terug op NO_STORAGE). Het sessiegedrag op regelniveau staat in `test/rdm_vectors/mbxd_session.json` (par. 3.17).

### 3.17 Conformance- en sessievectoren

`test/rdm_vectors/mailbox_conformance.json`: lijst van cases; elke case is een lijst stappen `{"t": <sec>, "op": "store|reg|fetch|stat|owner_add|deny|advance", "args": {...}, "expect": {...}}`. Hex-strings voor binaire velden. Draaien in de conformance-runner van `meshcore-mailboxd` (`cargo test`, zonder seriële poort) en in `test_rdm_mailbox_conformance` (tegen `MemMailboxBackend`). Een case die in één van beide faalt, blokkeert WP7 en WP8.

`test/rdm_vectors/mbxd_session.json`: het sessieprotocol van par. 3.16 op regelniveau. Elke case is een lijst stappen van twee soorten: een gebeurtenis `{"op": ..., "t": <sec>, "args": {...}}` aan één kant (`pi_start`, `owner_add`, `deny`, `undeny`, `radio_boot`, `radio_ms`, `radio_reg`, `assert_radio`) of een regelstap `{"from": "pi"|"radio", "lines": [...]}`: de kant die `from` noemt, moet precies die regels hebben gestuurd sinds zijn vorige regelstap; de andere kant krijgt ze als invoer. `players` per case zegt welke kant de case kan spelen (standaard beide): een v1-HELLO kan alleen de daemonfixture invoeren, een `mbx.ready 3` alleen de radiotest. Consumenten: `test_serial_pi_backend.cpp` (radiokant) en de golden fixtures van `meshcore-mailboxd` (daemonkant). Het bestand beschrijft het formaat zelf onder `format`.

## 4. Mappen en bestanden

```
src/Mesh.cpp                                   (WP5, flagged)
src/helpers/BaseChatMesh.{h,cpp}               (WP5, flagged)
src/helpers/rdm/                               kern: platformvrij, geen MeshCore.h/Utils.h/Packet.h/Arduino.h/Stream.h
  RdmTypes.h RdmConfig.h                       (WP0)
  RdmBytes.h RdmPolicy.h                       (intern, refactorronde 2)
  Rdm{Crypto,Codec}.{h,cpp}                    (h: WP0, cpp: WP1)
  Rdm{Storage,Clock,Contacts}.{h,cpp}          (h: WP0, cpp: WP2)
  RdmOutbox.{h,cpp}                            (h: WP0, cpp: WP3)
  Rdm{Inbox,Fetcher}.{h,cpp}                   (h: WP0, cpp: WP4)
  RdmNode.{h,cpp}                              (h: WP0, cpp: WP5)
src/helpers/rdm/mesh/RdmChatMesh.{h,cpp}       integratielaag op BaseChatMesh (h: WP0, cpp: WP5)
src/helpers/rdm/arduino/ArduinoFileIO.{h,cpp}  platformadapter FileIO (WP2)
src/helpers/rdm/mailbox/MailboxCore.{h,cpp}    serverrol (h: WP0, cpp: WP7)
examples/companion_radio/...                   (WP6)
examples/mailbox_server/*.{h,cpp}              (WP7)
examples/mailbox_server/mbxd/                  (WP8)
test/rdm_support/                              (per bestand, zie par. 5)
test/rdm_vectors/                              (WP8)
test/test_rdm_*/                               (per module, zie par. 5)
test/sim/, test/test_sim_*/                    (dm-current, niet aanraken)
tools/rdm/                                     (WP10; integrate.sh: tech lead)
```

De map bepaalt welke build een bestand compileert; de envs noemen de mappen in `build_src_filter` (een `*.cpp`-patroon matcht niet recursief):

| env | `build_src_filter` (RDM-deel) |
|---|---|
| `*_companion_radio_ble_rdm` (Heltec V3, RAK4631, T-Beam SX1262) | `+<helpers/rdm/*.cpp> +<helpers/rdm/mesh/*.cpp> +<helpers/rdm/arduino/*.cpp>` |
| `Heltec_v3_mailbox_server` | `+<helpers/rdm/mailbox/*.cpp> +<helpers/rdm/RdmCodec.cpp> +<helpers/rdm/RdmCrypto.cpp>` (alleen wat `MailboxCore` gebruikt) |
| `native_rdm` | de vier mappen, plus `-D RDM_SIM_FS` |

Daardoor staan `RdmNode.cpp` en `RdmChatMesh.cpp` niet meer achter een bestandsbrede `#ifdef WITH_RELIABLE_DM`. `ArduinoFileIO` compileert met `ARDUINO` of `RDM_SIM_FS`; in `native_rdm` via het `RP2040_PLATFORM`-pad op de `fs::FS`-shim van de simulator (`test/sim/shim/FS.h`).

Gedeelde testhelpers: `test/rdm_support/TestUtil.h` (patronen, sleutels, hex, LCG), `test/rdm_support/RdmTestStack.h` (inbox, register en contacten op de paden en recordgroottes van `Node`), `test/sim/CompanionFrames.h` (companion-codes en -frames; fork-codes uit `RdmCompanionProto.h`). Eigen suite: `test/test_rdm_internal/`.

## 5. Werkpakketten

Elk bestand heeft precies één eigenaar. Een WP die een bestand van een ander nodig heeft, gebruikt alleen de vastgelegde interface uit par. 3.

| WP | naam | bezit | hangt af van | klaar als |
|---|---|---|---|---|
| WP0 | Fundament | alle `src/helpers/rdm/*.h` uit par. 3; `platformio.ini`: regel `test_rdm_*` in `test_ignore` van `[env:native]` en nieuwe sectie `[env:native_rdm]`; `test/rdm_support/MemFileIO.{h,cpp}`, `test/rdm_support/SimFileIO.h`; `test/test_rdm_headers/` | `[env:native_sim]` van dm-current | elke header compileert los; `pio test -e native_rdm -f test_rdm_headers` groen; `MemFileIO` met foutinjectie (afgebroken schrijfactie na N bytes, mislukte create, corruptie, alles wissen, capaciteit); `SimFileIO` respecteert `SimFS::capacity_bytes` en `write_budget` |
| WP1 | Crypto en codec | `RdmCrypto.cpp`, `RdmCodec.cpp`, `test/test_rdm_crypto/`, `test/test_rdm_codec/` | WP0 | alle unit tests groen; `ACK_R` gelijk aan `BaseChatMesh::composeMsgPacket` voor 20 willekeurige berichten; grenzen 157/152 bewezen |
| WP2 | Opslag, klok, contacten | `RdmStorage.cpp`, `RdmClock.cpp`, `RdmContacts.cpp`, `ArduinoFileIO.{h,cpp}`, `test/test_rdm_storage/`, `test/test_rdm_clock/`, `test/test_rdm_contacts/` | WP0 | torn-write op elke byte-positie laat een geldige kopie; `Clock::millisUntil` is nooit te laat (ook over secondegrenzen) en `nextPersistMillis` klopt met het moment van schrijven (H4); `ArduinoFileIO` compileert voor ESP32 (SPIFFS) en nRF52 (LittleFS) in een env-build van WP6 |
| WP3 | Outbox | `RdmOutbox.cpp`, `test/test_rdm_outbox/` | WP0; linkt met WP1, WP2 | elke overgang uit `03` par. 4 en 11 (G1-G16) heeft een test; schema's met nepklok exact op de seconde. G16 (eerste actie van elke nieuwe RETRY-reeks 60-120 s na de time-out) gebruikt `RDM_RETRY_FIRST_MIN_S`/`RDM_RETRY_FIRST_MAX_S` uit `RdmConfig.h`. H4: na elke overgang doet `loop(nextDue - 1)` niets en `loop(nextDue)` wel |
| WP4 | Inbox en fetcher | `RdmInbox.cpp`, `RdmFetcher.cpp`, `test/test_rdm_inbox/`, `test/test_rdm_fetcher/` | WP0; linkt met WP1, WP2 | ACK pas na geslaagde write (schrijffout = geen ACK); sync-regel; watermerk; rapporten tot bevestiging; H4: `Fetcher::loop(nextDue - 1)` zendt niets, `loop(nextDue)` wel |
| WP5 | Node en mesh-integratie | `RdmNode.cpp`, `RdmChatMesh.cpp`, `src/Mesh.cpp`, `src/helpers/BaseChatMesh.{h,cpp}`, `test/test_rdm_node/`, `test/rdm_support/RdmSimCompanion.{h,cpp}` | WP1-WP4, simulator | `test_rdm_node` groen; scenario S01 groen in de simulator met twee `RdmSimCompanion`s; H4: `nextWakeupMillis` is het minimum van de onderdelen en nooit te laat, en S02 geeft met en zonder fast-forward een identieke `tx_log` |
| WP6 | Companion | `examples/companion_radio/{main.cpp, MyMesh.h, MyMesh.cpp, DataStore.h, DataStore.cpp, RdmCompanionProto.h}`, `variants/{heltec_v3,rak4631,lilygo_tbeam_SX1262}/platformio.ini`, `test/test_rdm_companion_proto/` | WP5 | envs `Heltec_v3_companion_radio_ble_rdm`, `RAK_4631_companion_radio_ble_rdm`, `Tbeam_SX1262_companion_radio_ble_rdm` en `Heltec_v3_mailbox_server` (blokken hieronder) bouwen; statuskanaal achter `RDM_STATUS_CHANNEL` bouwt; frame-tests groen |
| WP7 | Mailbox-firmware | `MailboxCore.cpp`, `examples/mailbox_server/{main.cpp, MailboxMesh.h, MailboxMesh.cpp, SerialPiBackend.h, SerialPiBackend.cpp}`, `test/test_rdm_mailbox_core/`, `test/rdm_support/RdmSimMailbox.{h,cpp}` | WP0, WP1, WP8 (`MemMailboxBackend`) | core-tests groen met `MemMailboxBackend`; `nextWakeupMillis` dekt de 3 s-backend-time-out; `SerialPiBackend` getest tegen opgenomen regels (par. 3.16); env bouwt |
| WP8 | mbxd en referentie-backend | `examples/mailbox_server/mbxd/{mbxd.py, test_mbxd.py, mbxd.service, README.md}`, `test/rdm_vectors/mailbox_conformance.json`, `test/rdm_support/MemMailboxBackend.{h,cpp}`, `test/test_rdm_mailbox_conformance/` | WP0 | `pytest` en `test_rdm_mailbox_conformance` groen op dezelfde vectoren; crash tussen insert en commit geeft geen `mbx.store` |
| WP9 | Scenario-integratietests | `test/test_rdm_scenarios/`, `test/rdm_support/{RdmSimApp.h, RdmScenario.h, RdmScenario.cpp, SubprocessBackend.h, SubprocessBackend.cpp}` | WP5, WP6 (proto), WP7, WP8, simulator | par. 6.2 volledig groen, inclusief S07 met echte `mbxd` via `SubprocessBackend` |
| WP10 | Compat-bewijs | `tools/rdm/check-upstream-identical.sh`, `tools/rdm/run-all-tests.sh`, `tools/rdm/check-headers.py` | WP5, WP6 | `check-upstream-identical.sh` bouwt de base (`22baa5e3`) en de werkboom na elkaar op hetzelfde pad met vaste `SOURCE_DATE_EPOCH` (RadioLib, Crypto en de ESP32-core bakken `__DATE__`/`__TIME__` in) en toont voor de drie companion-envs en voor `native`/`native_kiss_modem` identieke objecten, identieke preprocessor-uitvoer (`-E -P`, plus `.d`-bestanden) van `Mesh.cpp`, `BaseChatMesh.cpp`, `MyMesh.cpp`, `DataStore.cpp` en elke andere gewijzigde bron, en identieke firmware-images. Objecten die alleen in debug-info verschillen (vergeleken na `--strip-debug` met de objcopy van de juiste toolchain) en ESP32-images die alleen in `app_elf_sha256` en de image-hash verschillen, tellen als CODE-IDENTICAL (besluit WP5). `--compare-only` hergebruikt een eerdere build. `run-all-tests.sh` draait alle `native*`-envs en de pytest van `mbxd`, met samenvatting |
| extern | Simulator (dm-current) | `test/sim/*`, `test/test_sim_*/`, `[env:native_sim]`, `07-simulator.md` | - | eisen in par. 7 |

Integratie: `tools/rdm/integrate.sh [--dry-run] [--no-test] <worktree> [wp...]` kopieert de bestanden van een WP uit diens worktree naar de hoofdcheckout (eigen mappen gespiegeld, met backup), waarschuwt bij gewijzigde WP0-headers en bij bestanden buiten het eigendom, en draait daarna `native_rdm`, `native_sim` en (als aanwezig) de pytest van `mbxd`.

Kritisch pad: WP0 -> WP1/WP2 (parallel) -> WP3/WP4 (parallel) -> WP5 -> WP6 en WP9. WP7 en WP8 lopen vanaf WP0 parallel. WP3 en WP4 kunnen al starten met alleen de headers; hun tests linken pas als WP1 en WP2 klaar zijn.

`[env:native_rdm]` (WP0):

```ini
[env:native_rdm]
extends = env:native_sim
test_filter = test_rdm_*
build_flags = ${env:native_sim.build_flags}
  -D WITH_RELIABLE_DM
  -D WITH_DM_MAILBOX
  -I test/rdm_support
  -I examples/mailbox_server
build_src_filter = ${env:native_sim.build_src_filter}
  +<../src/helpers/rdm/*.cpp>
  +<../test/rdm_support/*.cpp>
  +<../examples/mailbox_server/MailboxMesh.cpp>
  +<../examples/mailbox_server/SerialPiBackend.cpp>
```

`ArduinoFileIO.cpp` staat achter `#ifdef ARDUINO` zodat hij in `native_rdm` leeg compileert. `RdmChatMesh.cpp` en `RdmNode.cpp` staan geheel achter `#ifdef WITH_RELIABLE_DM`, zodat de mailbox-build (alleen `WITH_DM_MAILBOX`) `helpers/rdm/*.cpp` kan blijven bouwen.

`[env:native_rdm_status]` (besluit WP6) draait `test_rdm_companion_proto` opnieuw met `-D RDM_STATUS_CHANNEL`.

Env-blokken in `variants/heltec_v3/platformio.ini` (WP6 plakt ze, WP7 levert de inhoud van `examples/mailbox_server`); `rak4631` en `lilygo_tbeam_SX1262` krijgen hetzelfde companion-patroon op hun `*_companion_radio_ble`-env:

```ini
[env:Heltec_v3_companion_radio_ble_rdm]
extends = env:Heltec_v3_companion_radio_ble
build_flags =
  ${env:Heltec_v3_companion_radio_ble.build_flags}
  -D WITH_RELIABLE_DM
build_src_filter = ${env:Heltec_v3_companion_radio_ble.build_src_filter}
  +<helpers/rdm/*.cpp>

[env:Heltec_v3_mailbox_server]
extends = Heltec_lora32_v3
build_flags =
  ${Heltec_lora32_v3.build_flags}
  -D DISPLAY_CLASS=SSD1306Display
  -D ADVERT_NAME='"Mailbox"'
  -D LORA_CR=8                    ; geen CLI (poort is van mbxd): CR komt uit de build; FREQ/BW/SF uit arduino_base
  -D WITH_DM_MAILBOX
;  -D MBX_FLOOD_SCOPE='"<regio>"'  ; alleen als de repeaters unscoped floods weigeren
build_src_filter = ${Heltec_lora32_v3.build_src_filter}
  +<helpers/ui/SSD1306Display.cpp>
  +<helpers/rdm/*.cpp>
  +<../examples/mailbox_server>
lib_deps =
  ${Heltec_lora32_v3.lib_deps}
```

## 6. Testmatrix

### 6.1 Unit tests (`pio test -e native_rdm`)

| suite | module | tests (minimaal) |
|---|---|---|
| `test_rdm_crypto` | Crypto | `ACK_R` gelijk aan upstream-berekening; `ACK_S` en `K` gelijk voor attempt 0-3 en 252+c; `ACK_S` ≠ `ACK_R`; `token_B` tegen een met Python `hmac` berekende vector; `txtPacketHash` gelijk aan `Packet::calculatePacketHash` |
| `test_rdm_codec` | Codec | round-trip van elk formaat in 3.4; DEPOSIT met 152 tekens past (plaintext 174), 153 niet; standaard-trailer (`0x00 | attempt`) geeft `cap = false`; cap-ACK herkend, PATH-extra met nulpadding niet; alle parsers weigeren afgekapte input; 0x91-frame 20 bytes |
| `test_rdm_storage` | RecordFile | create/open/reopen; afgebroken schrijfactie op elke byte-positie laat de vorige waarde leesbaar (A/B); CRC-fout in één record laat de rest intact; migratie van versie 1 naar 2; `fileSize` |
| `test_rdm_clock` | Clock | monotoon over reboot zonder RTC (D1); betrouwbare RTC vooruit neemt over, achteruit genegeerd; persist-interval; `nextReqTimestamp` strikt stijgend over reboot; nieuwe `store_id` alleen bij recreate |
| `test_rdm_contacts` | ContactTable | find/add/evict-regel; cap zetten en wissen; `findByMailbox` |
| `test_rdm_outbox` | Outbox | elke overgang van `03` par. 4; G1-G5, G8-G11, G13-G15; app-retry op dezelfde entry met nieuwe `app_ack`; firmware-attempts 252+c aflopend en wrap; WAIT_ACK-regel `max(2 × est, 30 s)`; probe-schema, flood hooguit elke 4 u, DM elke 24 u ook met cap; foute ack genegeerd; slotvrijgave en NO_OUTBOX; `unreported`/`confirm_pending`; herstel na reboot met herberekende ACK's |
| `test_rdm_inbox` | Inbox | NEW/DUPLICATE/INBOX_FULL/STORE_ERROR; quotum per afzender; geen ACK bij schrijffout; sync pas bij volgend sync-verzoek; verbinding weg geeft heraanbieding; eviction alleen van gesyncte entries; watermerk geeft EVICTED; query-states met juiste ack; rapporten blijven tot bevestiging |
| `test_rdm_fetcher` | Fetcher | boot-jitter binnen 0-60 s; 60-min-interval; FETCH na sync; advert-trigger min 10 min; direct door bij `resterend > 0`; NO_PAYLOAD bij volle inbox; `store_id` in elke FETCH |
| `test_rdm_node` | Node | twee `Node`s aan een nep-draad (geen `Mesh`): afzender tot DELIVERED, ontvanger toont 1x; runtime-uit geeft upstreamgedrag |
| `test_rdm_companion_proto` | companion-frames | encode/decode van 3.14 |
| `test_rdm_mailbox_core` | MailboxCore | alle DEPOSIT-codes; 3 s zonder backend = NO_STORAGE; rate-limits; replaycheck; flood-FETCH -> PATH-return zonder kopie; TOO_BIG zonder backend-aanroep; registratie |
| `test_rdm_mailbox_conformance` | MemMailboxBackend | alle cases uit de JSON |
| `test_mbxd.py` (pytest) | mbxd | alle cases uit de JSON; regelparser; commit voor antwoord; herstart behoudt stand; TTL; quotum; token en denylist; `store_id`-wissel = RESYNC; SENT zonder rapport terug naar STORED |

### 6.2 Integratietests in de simulator (`test/test_rdm_scenarios/`)

Opstelling tenzij anders vermeld: Bob (B) en Alice (A) als `RdmSimCompanion` met `RdmSimApp`, repeater R ertussen (B-R-A, geen directe link), mailbox M als `RdmSimMailbox` met `MemMailboxBackend`, gelinkt aan R. Beide companions zonder RTC-batterij; de app zet de klok bij verbinden (`sync_clock`). Zendtijd-asserts gebruiken een tweede topologie zonder repeater (alle nodes direct gelinkt, behalve waar het scenario een link verbiedt) en vergelijken met `03` par. 5 of `05` binnen 20%.

| test | scenario | offline-momenten | verwachte statussen bij Bob (0x91) | asserts |
|---|---|---|---|---|
| `S01_Basis` | 1 | geen; pad bekend (zoals `05`) | QUEUED, ON_RADIO, DELIVERED | `SEND_CONFIRMED` 1x met `app_ack`; A-app ziet het bericht 1x; A zendt `ACK_R` pas na inbox-write (volgorde in FS-log); zendtijd 2,3 s ± 20%. Variant zonder pad: ON_RADIO mag ontbreken (G8), eindstatus DELIVERED en 1x `SEND_CONFIRMED` |
| `S02_AliceTelefoonWeg` | 2 | A-app weg tot +13 u (WP5-versie: +6 u) | QUEUED, ON_RADIO, DELIVERED | queries op 1 u en 5 u na ON_RADIO (tussenpozen 1 u, 4 u; ± 1 s); na sync `ACK_S` los; variant met gedropt `ACK_S`: DELIVERED via volgende query; zendtijd 4,7 s ± 20% (telefoon 1 dag weg) |
| `S03_AliceRadioUit` | 3 | A power_off voor verzenden, aan na 5 u | QUEUED, ON_RADIO, DELIVERED | app-retry landt op dezelfde entry; probes elke 30 min; eerste probe na aangaan geeft UNKNOWN en de DM volgt binnen 60 s; firmware-attempt 252+c; A toont 1x |
| `S04_BobWegVoorAckS` | 4 | B power_off na ON_RADIO; A synct terwijl B uit is; B aan na 2 u | ON_RADIO, DELIVERED (na herverbinden als `unreported`) | boot-kick-query binnen 60-120 s na boot; variant met gevuld register (EVICTED) eindigt ook in DELIVERED; 0x91 DELIVERED pas na `CMD_RDM_ENABLE` |
| `S05_NooitTegelijk` | 5 | B en A afwisselend aan, nooit overlap, 8 dagen | QUEUED, EXPIRED | EXPIRED na T_radio (7 d RDM-tijd, B continu aan); geen DM bij A; aantal probes volgens schema (dag 1: 48 op 90 s + k × 1800 s, plus een hele DM op 24 u door G15); slot vrij 24 u na EXPIRED als niet gerapporteerd |
| `S06_AliceVerliestInbox` | 6 | A-app weg; A-bestanden `/rdm/inbox` en `/rdm/register` gewist na `ACK_R` | QUEUED, ON_RADIO, QUEUED, ON_RADIO, DELIVERED | query geeft UNKNOWN, B stuurt opnieuw, A toont 1x; variant G7 (gesynct, `ACK_S` verloren, dan wissen): duplicaat toegestaan en gelogd (D2) |
| `S07_Mailbox` | 7 | A uit bij verzenden; B uit na CUSTODY; A aan (FETCH), A-app later; B aan | QUEUED, CUSTODY, DELIVERED | geen directe link B-A; registratie via G9 (NOT_AUTH, of twee onbeantwoorde DEPOSIT's bij een Bob die M nog niet kent), REG OK met `tijd_M` = Pi-klok; `mbx.store` voor STORED; FETCH flood -> PATH -> FETCH direct met kopie; rapporten ON_RADIO en SYNCED bevestigd; B controleert `ACK_S` zelf; zendtijd 7,9 s ± 20%. Variant `S07_MailboxEchteMbxd`: zelfde met `SubprocessBackend` (python3 `mbxd` via stdin/stdout) |
| `S08_MailboxResync` | 8 | A wist RDM-opslag na ON_RADIO-rapport, voor app-sync | CUSTODY, ON_RADIO, DELIVERED | nieuwe `store_id` in FETCH; M levert opnieuw met andere tag (andere packet hash); A toont 1x |
| `S09a_MailboxOnbereikbaar` | 9a | backend in modus "geen antwoord"; A uit, later aan | QUEUED, ON_RADIO, DELIVERED | NO_STORAGE na 3 s; B nooit CUSTODY; re-deposit volgens G13; probe levert direct af |
| `S09b_MailboxAchterhaald` | 9b | backend in modus "houdt achter" (FETCH zonder kopie); B-A gelinkt | QUEUED, CUSTODY, ON_RADIO, DELIVERED | probe in CUSTODY na 24 u geeft UNKNOWN en een directe DM; latere FETCH met kopie wordt DUPLICATE, A toont 1x |
| `S10_AliceStandaard` | 10 | variant A uit met backoff-DM's | QUEUED, ON_RADIO_FINAL | A met `Node::setEnabled(false)`: ACK zonder cap-byte, byte-gelijk aan upstream; geen `RECEIPT_QUERY`, geen MBX_INFO; B stuurt na ON_RADIO_FINAL niets meer |
| `S11_CapIngetrokken` | G15 | A eerst fork, daarna `setEnabled(false)`, A tijdelijk uit | QUEUED, ON_RADIO_FINAL | B stuurt binnen 24 u een hele DM ondanks bekende cap; cap gewist na ACK zonder cap-byte |
| `S12_MailboxWissel` | G14, G17 | A wisselt van M1 naar M2 terwijl B in CUSTODY bij M1; A na Bobs ACK op de nieuwe MBX_INFO onbereikbaar, A-app synct na Bobs ON_RADIO | QUEUED, CUSTODY, QUEUED, CUSTODY, ON_RADIO, DELIVERED | MBX_INFO herhaald tot B ACKt; B deponeert bij M2; kopie bij M1 wordt nooit getoond |
| `S14_HiddenTerminalVerlorenAck` | G16, `07` par. 6 | geen; B-R-A, tweede DM direct na de eerste ACK zodat Alice' flood-ACK bij R botst (of `drop_next(ACK)`) | QUEUED, ON_RADIO, DELIVERED | eerste RETRY-actie 60-120 s na de time-out; bij bekende cap een probe met ON_RADIO-antwoord, zonder cap een DM die Alice' register als duplicaat vangt; A toont het bericht 1x; `SEND_CONFIRMED` 1x |
| `S13_StandaardApp` | beslissing 6, I8 | app zonder `CMD_RDM_ENABLE` | alleen `SEND_CONFIRMED` | geen 0x91-frames naar deze app; `SEND_CONFIRMED` 1x bij ON_RADIO; bij een app die pas na ON_RADIO verbindt alsnog 1x (G5) |

Invulling uit WP9: S05 laat Alice 6 u aan en 18 u uit met haar link weg, Bob continu aan. In S09a is Bob vooraf bij M geregistreerd (anders ontsleutelt M zijn DEPOSIT niet en komt er geen NO_STORAGE). In S09b ontstaat DUPLICATE alleen als de kopie van M vóór de app-sync binnenkomt; daarna is SYNCED correct. Zendtijd van S07 telt de pakketten van `03` par. 5, niet de registratie en lege uur-FETCH's.

Schema-asserts met G17: elke tussenpoos I ligt in [I, I + min(I/10, 60 s)] (+1 s afronding); tellingen per etmaal krijgen de bijbehorende marge (dag 1: 45-48 probes). Zendtijd-asserts van S01/S02 zitten in WP9 (`S01_Zendtijd`, `S02_Zendtijd`), de gedragsscenario's in `test_rdm_node`.

Tijdsprong: scenario's van dagen gebruiken de fast-forward van de simulator (`set_fast_forward`, par. 7).

S01 en S02 zijn door WP5 gebouwd in `test/test_rdm_node/test_rdm_sim.cpp` (S02 ingekort tot 6 u, twee queries); WP9 dupliceert ze niet en verwijst ernaar.

### 6.3 Eindstand v1 (29 sep 2026, integratieronde 8)

Alles in de hoofdcheckout, niet gecommit. `tools/rdm/run-all-tests.sh`: ALL PASSED.

| env | testgevallen |
|---|---|
| native | 46/46 (upstream; `test_companion_node_prefs` staat upstream uit) |
| native_kiss_modem | 8/8 (upstream) |
| native_sim | 22/22 (simulator zelf, upstream-gedrag, L1/L3/L6/#3518 reproduceerbaar) |
| native_rdm | 431/431 |
| native_rdm_status | 38/38 (companion-suites met `RDM_STATUS_CHANNEL`) |
| pytest `mbxd` | 86 passed |

Scenario's uit `05` en par. 6.2, alle groen:

| scenario | test(s) | bestand |
|---|---|---|
| S01 basis | `RdmSim.S01_Basis`, `RdmSim.S01_FirstDmWithoutPathEndsDelivered`, `RdmSim.S01_AirtimePerHop`, `RdmScenario.S01_Zendtijd` | `test/test_rdm_node/test_rdm_sim.cpp`, `test/test_rdm_scenarios/test_rdm_scenarios.cpp` |
| S02 telefoon weg | `RdmSim.S02_FastForwardGivesTheSameRun`, `RdmScenario.S02_AliceTelefoonWeg_AckSVerloren`, `RdmScenario.S02_Zendtijd` | idem |
| S03 radio uit | `RdmScenario.S03_AliceRadioUit`, `..._CompanionNode` | `test_rdm_scenarios.cpp` |
| S04 Bob weg | `RdmScenario.S04_BobWegVoorAckS`, `..._GevuldRegister` (EVICTED) | idem |
| S05 nooit tegelijk | `RdmScenario.S05_NooitTegelijk`, `..._SlotNaVierentwintigUur` | idem |
| S06 inbox kwijt | `RdmScenario.S06_AliceVerliestInbox`, `..._G7Duplicaat` (D2) | idem |
| S07 mailbox | `RdmScenario.S07_Mailbox`, `..._CompanionNode`, `S07_MailboxEchteMbxd` (echte `mbxd` via `SubprocessBackend`) | idem |
| S08 RESYNC | `RdmScenario.S08_MailboxResync` | idem |
| S09a/b | `RdmScenario.S09a_MailboxOnbereikbaar`, `S09b_MailboxAchterhaald` | idem |
| S10 standaard Alice | `RdmScenario.S10_AliceStandaard`, `..._Backoff`, `RdmSim.StockAliceGetsAnUpstreamAckWithoutCap` | `test_rdm_scenarios.cpp`, `test_rdm_sim.cpp` |
| S11 cap ingetrokken | `RdmScenario.S11_CapIngetrokken` | `test_rdm_scenarios.cpp` |
| S12 mailboxwissel (G14, G17, met botsingen) | `RdmScenario.S12_MailboxWissel` | idem |
| S13 standaard app | `RdmScenario.S13_StandaardApp`, `..._CompanionNode`, `RdmCompanion.StandardAppGetsSendConfirmedButNoStatusPushes` | `test_rdm_scenarios.cpp`, `test/test_rdm_companion_proto/test_companion_mymesh.cpp` |
| S14 hidden terminal (G16) | `RdmScenario.S14_HiddenTerminalVerlorenAck` | `test_rdm_scenarios.cpp` |
| K3 mailbox geen contact | `RdmCompanion.MailboxLeavesTheContactsWhenItBecomesAHiddenPeer`, K3-assert in de S07-varianten | `test_companion_mymesh.cpp`, `test_rdm_scenarios.cpp` |
| statuskanaal | `RdmStatusChannel.OnRadioAndDeliveredOnceNoQueuedNoReplayDuplicate` | `test_companion_mymesh.cpp` (`native_rdm_status`) |

Unit-suites per module (par. 6.1): `test_rdm_crypto`, `test_rdm_codec`, `test_rdm_storage`, `test_rdm_clock`, `test_rdm_contacts`, `test_rdm_outbox` (91), `test_rdm_inbox`, `test_rdm_fetcher`, `test_rdm_node`, `test_rdm_companion_proto`, `test_rdm_mailbox_core` (MailboxCore, SerialPiBackend, mailbox-sim), `test_rdm_mailbox_conformance`, `test_rdm_headers`; plus `test_scenario_support.cpp` (`MbxdFixture`, `WireLog`).

Firmware (`pio run`): `Heltec_v3_companion_radio_ble_rdm` (RAM 59,4%, flash 39,7%), idem met `RDM_STATUS_CHANNEL`, `RAK_4631_companion_radio_ble_rdm` (RAM 72,7%, flash 73,1%), `Tbeam_SX1262_companion_radio_ble_rdm` (flash 77,8%) en `Heltec_v3_mailbox_server` (RAM 14,2%, flash 18,2%): alle SUCCESS.

Compat (`tools/rdm/check-upstream-identical.sh` tegen `22baa5e3`): CODE-IDENTICAL. RAK 4631, native en native_kiss_modem byte-identiek; Heltec V3 en T-Beam alleen debug-info en de daarvan afgeleide ELF/image-hashes.

Nog open na v1: HIL (par. 8), de drie open punten over de standaard app (`03` par. 9), commit (wacht op signing).

## 7. Afstemming met de simulator (dm-current)

Stand volgens `07-simulator.md` par. 7: alle zes eisen zijn ingevuld.

| # | eis | invulling | gevolg voor RDM |
|---|---|---|---|
| S1 | `SimNode::fs()` en `SimFS::files` bewerkbaar | ja, plus `capacity_bytes` en `fail_writes_after` | `SimFileIO` (WP0) gebruikt beide |
| S2 | eigen `mesh::Mesh` via `createMesh()` | ja; `beginMesh()`/`loopMesh()` overriden omdat `Mesh::begin()` en `BaseChatMesh::loop()` niet virtueel zijn | `RdmSimCompanion` en `RdmSimMailbox` volgen dat patroon, plus `busy()`/`nextWakeupMs()` voor fast-forward |
| S3 | fast-forward | `set_fast_forward(max_step, settle)` | `RDM_TEST_TIME_DIVISOR` vervalt; scenario's van dagen draaien in echte RDM-tijd. `Node` moet via zijn sim-node `nextWakeupMs()` zijn vroegste deadline melden |
| S4 | airtime per TX-record | `TxRecord::airtime_ms`, `seq` | zendtijd-asserts direct uit `tx_log` |
| S5 | één pakket laten vallen | `drop_next`, `add_drop_filter` | verloren `ACK_S`, FETCH-antwoord en `ACK_R` (S14) |
| S6 | echte companion | `CompanionNode` met `CompanionApp`, compileert in `native_rdm` met `-D WITH_RELIABLE_DM` | WP9 gebruikt `CompanionNode` voor Bob en Alice zodra WP6 er is; daarvoor `RdmSimCompanion` |

Bevindingen uit de simulator die het ontwerp raken: de klok van de companion na een reboot (G6, D1) en de verloren ACK na geslaagde aflevering (G16); beide verwerkt in `03` par. 11. De queue-grootte in `01` is gecorrigeerd (256 op Heltec V3 BLE).

## 8. Na v1

- HIL met twee companions (Heltec V3) en de mailbox-Heltec met Pi: scenario's 1, 2, 3, 7 en 10 op 869.618/62.5/SF8, plus de drie open punten over de standaard app (`03` par. 9).
- Zendtijdmeting op echte radio's naast `03` par. 5.
- Daarna pas de GitHub-discussie (beslissing 5).

## 9. Risico's

| risico | gevolg | maatregel |
|---|---|---|
| Simulator-eis S2 of S3 laat op zich wachten | WP9 staat stil | `test_rdm_node` (nep-draad) dekt de protocolketen al; WP9 start met S01 zodra S2 er is |
| Semantiekverschil `mbxd` tegenover `MemMailboxBackend` | groene sim, rode Pi | gedeelde conformance-vectoren blokkeren beide WP's; S07 draait ook tegen echte `mbxd` |
| Flash op nRF52 zonder extra FS te klein | RDM uit op die borden | `RDM_MIN_FREE_BYTES`, fallback naar standaard; gemeten in WP6 |
| `RdmChatMesh` dupliceert een stuk van `BaseChatMesh::onPeerDataRecv` (plain-TXT-tak) | afwijking bij upstream-wijziging | WP10 vergelijkt; test in `test_rdm_node` dat een DM zonder trailer exact dezelfde ACK geeft als upstream |
| Standaard app reageert onverwacht op nieuwe frames | app-fouten | 0x91 alleen na `CMD_RDM_ENABLE` (K9); HIL na v1 |
