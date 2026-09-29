# 03: Ontwerp betrouwbare DM's

Basis: `00-requirements.md`, `01-huidige-werking.md`, `02-prior-art.md`. Beslissing in `adr-001-reliable-dm.md`, review in `04-review.md`.
Omvang: groot en moeilijk omkeerbaar (wire-formaat, nieuwe noderol), dus opties en trade-offs expliciet.
Bijgewerkt 29 sep 2026: beslissingen van Andy (par. 9) en de review (B1-B2, I1-I10, Y1-Y2, K1-K11).

## Aanbeveling

**Optie B, gefaseerd: eerst een persistente outbox bij de afzender plus persistente inbox en dedup bij de ontvanger (A), daarna een dedicated mailbox (eigen Heltec + eigen Pi) die Alice kiest, die de originele versleutelde DM ongewijzigd bewaart en in het antwoord op Alice' ophaalverzoek meestuurt.**

- Wat het oplevert: aflevering zonder dat beide radio's ooit tegelijk aan hoeven te zijn, en drie statussen bij Bob: "in bewaring" (mailbox), "op Alice' radio" en "afgeleverd" (Alice' telefoon heeft het opgehaald). De laatste twee kan de mailbox of een repeater niet vervalsen.
- Wat het kost: drie firmwarerollen (companion-afzender, companion-ontvanger, mailbox), een daemon op een eigen Pi, een kleine wijziging in `src/Mesh.cpp` onder flag (par. 3). Zendtijd per afgeleverd mailboxbericht 7,9 s per hop, leeg ophalen 0,8 s per hop per uur (par. 5).
- Kantelpunt: zijn de radio's van Bob en Alice in de praktijk vrijwel altijd aan (vaste thuisnodes), dan levert A al bijna alles en voegt de mailbox alleen "in bewaring" toe. A vormt de eerste stappen van B, dus dit wordt gemeten voordat de mailbox gebouwd wordt.

## 1. Kernmechanismen

### a. Bewijzen zonder dat de mailbox ze kan maken

De bestaande ACK is `sha256(timestamp | flags | tekst, pubkey_Bob)[0:4]` (`src/helpers/BaseChatMesh.cpp:462`, `:244-245`); alleen wie de plaintext heeft kan hem berekenen. Ontwerpregel: **Bob geeft zijn verwachte ACK's nooit aan M.** Correlatie met M gaat via de packet hash `SHA256(0x02 | payload)[0:8]` (`src/Packet.cpp:41-50`); met Alice via de sleutel `K = SHA256(timestamp | tekst | pubkey_Bob)[0:4]`, die attempt-onafhankelijk is en die Bob en Alice allebei kunnen berekenen.

| bewijs | waarde | betekenis | wie het doorgeeft |
|---|---|---|---|
| `ACK_R` | bestaande ACK, 4 bytes, hangt af van `attempt & 3` (`BaseChatMesh.cpp:457-462`) | "op Alice' radio" | Alice direct, of M ongewijzigd |
| `ACK_S` | `SHA256("RDMS" | timestamp | (txt_type << 2) | tekst | pubkey_Bob)[0:6]`, attemptbits op 0 | "afgeleverd" | Alice direct, M ongewijzigd, of in een `RECEIPT_QUERY`-antwoord |

- Bob bewaart de tekst en berekent `ACK_R` voor alle vier klassen en `ACK_S` bij het laden in RAM; persistent staan ze niet (Y2).
- `ACK_S` is 48 bits: een derde die willekeurige ACK-pakketten uitzendt, raakt bij ~200 openstaande waarden met 10% duty cycle gemiddeld eens per ruim 100.000 jaar een treffer (1286 pakketten per uur, kans 200/2^48 per pakket; K1). Voor `ACK_R` blijft het 32 bits (bestaand formaat, ~0,5 treffer per jaar voor zo'n aanvaller); dat raakt alleen de tussenstatus.
- Een `RECEIPT_QUERY`-antwoord is een RESPONSE versleuteld en gemaakt met ECDH(Alice, Bob): het is zelf een bewijs van Alice.

### b. Capability in-band (B1)

Companions adverten alleen handmatig (`examples/companion_radio/MyMesh.cpp:1297`, `adv_int` uitgecommentarieerd in `NodePrefs.h:195-196`), dus een advert-bit komt zelden aan. Capability gaat daarom mee in het verkeer zelf; FEAT1 vervalt (YAGNI).

- **Bob -> Alice**: elke fork-DM krijgt na de tekst `0x00 | attempt | 0x81` (cap-byte, versie 1). Precedent is het extended attempt-byte (`BaseChatMesh.cpp:465-468`); een standaard ontvanger toont de tekst tot de NUL en ACKt normaal. Kost 3 bytes: maximaal 157 bytes tekst; langer gaat zonder trailer als gewone DM.
- **Alice -> Bob**: een fork-Alice voegt `0x81` toe achter de 6-byte ACK, als los ACK-pakket (7 bytes) en als PATH-extra. Oude nodes lezen alleen de eerste 4 bytes (`src/Mesh.cpp:78-87`, `BaseChatMesh.cpp:361-364`); PATH-extra van een standaard node is met nullen gepadded, dus `0x81` is onderscheidbaar. Ook een `RECEIPT_QUERY`-antwoord bewijst capability.
- Capability wordt per contact persistent bewaard (par. 6). Een vervalst cap-byte laat Bob hooguit wachten op een `ACK_S` die niet komt: SYNC_EXPIRED in plaats van ON_RADIO_FINAL.

### c. "Afgeleverd" = Alice' telefoon (beslissing 2)

- Alice stuurt `ACK_R` na persistente opslag in haar inbox, `ACK_S` zodra haar app het bericht gesynct heeft, en `ACK_S` alleen aan afzenders met cap-byte.
- **Sync**: `CMD_SYNC_NEXT_MESSAGE` haalt uit de queue zonder bevestiging (`MyMesh.cpp:1415-1423`). De fork telt een DM als gesynct zodra de app op dezelfde verbinding het volgende `CMD_SYNC_NEXT_MESSAGE` stuurt. Valt de verbinding eerder weg, dan wordt het bericht opnieuw aangeboden: mogelijk een duplicaat in de app, geen verlies.
- Weergave op het scherm van de companion zelf telt niet als sync (K11, conform beslissing 2); het inboxquotum per afzender (par. 6) begrenst het vollopen.
- De tussenstatus is nodig om zendtijd te sparen: na `ACK_R` stuurt Bob het bericht niet meer, alleen nog `RECEIPT_QUERY` (0,41 s in plaats van 1,72 s).

| Bob | Alice | maximum voor Bob |
|---|---|---|
| fork | fork | ON_RADIO en DELIVERED |
| fork | standaard | `ACK_R` zonder cap-byte, direct bij ontvangst in RAM: ON_RADIO_FINAL |
| standaard | fork | `ACK_R` na persistente opslag, geen `ACK_S`; standaard app ziet "delivered" zoals nu |
| standaard | standaard | ongewijzigd |

### d. Samenspel met de app (I8, beslissing 1)

- **Één entry per bericht**: outbox-sleutel is `(contact, timestamp, K)`. Een app-retry (zelfde timestamp en tekst, ander attempt, `MyMesh.cpp:1135`) landt op de bestaande entry. De companion antwoordt `RESP_CODE_SENT` met de `ACK_R` van dat attempt en onthoudt die als `app_ack`. Staat de entry al in CUSTODY of ON_RADIO, dan wordt er niets uitgezonden.
- **Firmware-attempts** tellen af vanaf `252 + c` (dan 248 + c, ...), met `c` de klasse van de eerste app-poging. Ze vallen dus niet samen met oplopende app-attempts (L9) en houden dezelfde `ACK_R`. Na 63 varianten wrapt de teller; een herhaalde hash na uren is onschadelijk (K7).
- **`PUSH_CODE_SEND_CONFIRMED`** draagt altijd `app_ack`, want daarop matcht de app (`MyMesh.cpp:417-434`). Hij gaat naar de app bij `ACK_R` (beslissing 6).
- `PUSH_CODE_RDM_STATUS` (0x91) gaat alleen naar een client die zich met `CMD_RDM_ENABLE` heeft aangemeld; de standaard app krijgt geen onbekende push codes (K9).
- Het lokale statuskanaal "rdm-status" (`RDM_STATUS_CHANNEL`) wordt pas gebouwd na de meting in stap 3. Kanaalframes worden bij een volle queue als eerste verdrongen (`MyMesh.cpp:224-241`), dus statusregels kunnen daar wegvallen.

### e. Dedup, replays, paden, opslag

| probleem | oplossing |
|---|---|
| Repeaters droppen een identieke herhaling (`src/Mesh.cpp:101-104`, ring 160) | Elke firmware-herhaling heeft een eigen attempt-byte. M zendt de kopie in een RESPONSE met unieke tag, dus elke levering heeft een unieke hash (Y1, I5). |
| Dubbele weergave bij Alice (L6) | Persistent ontvangstregister op sleutel `sender + K`, onafhankelijk van attempt. Duplicaat: `ACK_R` opnieuw, `ACK_S` als al gesynct, niet tonen. |
| Register vol (I7) | Alleen gesyncte entries worden verwijderd, oudste eerst; per afzender een laagwatermerk (hoogste verwijderde timestamp). Query op een onbekende sleutel met timestamp ≤ watermerk geeft EVICTED. Bob in ON_RADIO leest dat als DELIVERED (ongesyncte entries worden nooit verwijderd, en het antwoord komt van Alice zelf); Bob in een eerdere state stuurt het bericht, want Alice heeft het dan nooit gehad. |
| Pad-vervuiling (L8) | M zendt geen TXT_MSG. Alice verwerkt de kopie uit M's antwoord en stuurt Bob geen ACK; haar bewijzen gaan via M (Y1). Bobs `out_path` blijft onaangeroerd. |
| Kapot pad (L7) | Outbox en probes: na 2 mislukte direct-pogingen een flood-poging; het PATH-antwoord zet een vers pad. |
| 8-plekken ACK-tabel (L13) | `processAck` kijkt eerst in de outbox-RAM-cache. |
| #3518 | `ACK_R` pas na geslaagde persistente opslag; inbox vol geeft geen ACK. De kale fix (geen ACK bij volle RAM-queue) is een losse PR-kandidaat. |

## 2. Opties

| criterium | A: outbox bij afzender | B: A + dedicated mailbox (radio+Pi) | C: S&F op repeaters (edotassi) | D: room server die ciphertext bewaart |
|---|---|---|---|---|
| R3 E2E | ja | ja (ongewijzigde ciphertext) | ja | ja, als hij niet ontsleutelt |
| R5 "in bewaring" | nee | ja | nee (geen receipt, `02` par. 3) | ja |
| R5 "afgeleverd" onvervalsbaar | ja | ja (par. 1a) | ja, maar ACK bereikt offline Bob niet | ja, met par. 1a |
| R6 zonder mailbox | ja | ja (A zit erin) | ja | ja |
| R7 onbeperkte opslag | n.v.t. | ja (Pi) | nee (RAM-ring 500) | met Pi ja |
| Aflevering zonder gelijktijdige bereikbaarheid | nee | ja | alleen als de repeater bij Alice staat | ja |
| Zendtijd | probes 0,41 s, DM 1,72 s per hop | par. 5 | flood-replay bij elke advert | als B |
| Upstream-kans | hoog (#1834, #3518) | middel: nieuwe opt-in rol, geen tussennode | laag: afgewezen in #613 | laag: vermengt rollen, ACL (32) gedeeld |
| Bouwwerk | klein | groot | middel, op infra van anderen | als B plus ontvlechting |

Keuze B met A als eerste fase. C valt af op R7, R5 en #613; D is afgewezen (beslissing 3). De mailboxfirmware hergebruikt het skelet van de room server (`ClientACL`, ANON_REQ-login).

## 3. Protocolextensie

Geen versiebits, geen nieuwe payload types (`01` par. 9): TXT_MSG, REQ, RESPONSE, ANON_REQ en ACK, die standaard repeaters forwarden. Nummers voorlopig (beslissing 5). Little-endian.

**Exacte lengtecheck (I1).** `Mesh::createDatagram` weigert bij `data_len + 2 + 15 > 184` (`src/Mesh.cpp:490`), dus boven 167 bytes plaintext, terwijl `2 + 2 + pad16(data_len) ≤ 184` tot 176 toestaat. Onder `WITH_RELIABLE_DM`/`WITH_DM_MAILBOX` wordt de check exact. Grenzen daarmee:

| bericht | plaintext | maximale tekst |
|---|---|---|
| fork-DM met trailer | `5 + tekst + 3 ≤ 167` (ongewijzigde check volstaat) | 157 |
| mailboxkopie | DEPOSIT `4+1+4+1+(4+pad16(5+tekst+3)) ≤ 176`; FETCH-antwoord `4+1+1+1+164 = 171` | 152 |
| zonder exacte check | idem `≤ 167` | 136 |

Tekst tussen 153 en 157 bytes gaat alleen via A (status `TOO_BIG` voor de mailbox).

### Alice -> Bob: MBX_INFO (TXT_MSG, txt_type 8)

`flags = 0x20`. Standaard companions negeren onbekende txt_types stil (`BaseChatMesh.cpp:308-310`).
`timestamp(4) | 0x20 | sub(1) | versie(1) | mailbox pubkey(32) | token_B(8)` = 47 bytes, payload 52. sub `0x01` MBX_INFO, `0x02` MBX_REVOKE. Bob ACKt met `sha256(plaintext[0..47), pubkey_Alice)[0:4]`.

- `token_B = HMAC-SHA256(K_owner, pubkey_Bob)[0:8]` (I3). `K_owner` (16 bytes) kennen alleen Alice' radio en de Pi. Een uitgelekt token werkt alleen voor die ene pubkey; intrekken via een denylist op de Pi.
- Alice stuurt MBX_INFO alleen naar **favoriete** contacten (`ContactInfo::isFav`, `src/helpers/ContactInfo.h:29`), bij de eerste fork-DM van zo'n contact en bij een wijziging van de mailbox. Auto-added contacten krijgen niets.

### Bob -> M: registratie (ANON_REQ)

`timestamp(4) | 0xFF | 'M' | versie(1) | owner_prefix(4) | token_B(8)` = 19 bytes, payload 67 (`src/Mesh.cpp:512-538`). Antwoord: `tag(4) | status(1) | ttl_dagen(1) | quota(1) | tijd_M(4)`. `owner_prefix` is overal 4 bytes (K5).

### REQ-types

Alle REQ's: `timestamp(4) | req_type(1) | ...`; RESPONSE: `tag(4) | ...`.

| req_type | richting | rest van de REQ | RESPONSE na `tag(4)` |
|---|---|---|---|
| 0x41 DEPOSIT | Bob -> M | `owner_prefix(4) | inner_len(1) | inner payload` | `status(1) | pkt_hash(8) | expires(4)` |
| 0x42 FETCH | Alice -> M | `flags(1) | n(1) | n × (pkt_hash(8) | result(1) | ack(6))`, n ≤ 8 | `resterend(1) | rapporten_ok(1) | inner_len(1) | inner payload` |
| 0x44 STATUS | Bob -> M | `n(1) | n × pkt_hash(8)` | `n(1) | n × (state(1) | ack(6))` |
| 0x45 RECEIPT_QUERY | Bob -> Alice | `n(1) | n × (timestamp(4) | K(4))`, n ≤ 8 | `n(1) | n × (state(1) | ack(6))` |

- **inner payload**: de complete TXT_MSG-payload `dest | src | MAC | ciphertext` met een eigen firmware-attempt en de cap-trailer. M bewaart hem bit-voor-bit.
- **FETCH** (Y1): elk antwoord draagt hooguit één kopie; Alice rapporteert de verwerking in de volgende FETCH. FETCH-flags: `0x01` RESYNC (inbox opnieuw geformatteerd), `0x02` NO_PAYLOAD (inbox vol, alleen rapporten). Komt de FETCH via flood binnen, dan antwoordt M met een PATH-return zonder payload; die legt de paden vast, daarna gaat alles direct (een PATH met een 171-byte antwoord past niet).
- **Rapport result** (ack in 6 bytes, `ACK_R` aangevuld met 2 nullen): 0 ON_RADIO (`ACK_R`), 1 SYNCED (`ACK_S`), 2 DUPLICATE (`ACK_R`), 3 UNDECRYPTABLE (Alice kent Bob niet, L10; Alice ontsleutelt zelf, dus geen hook in `Mesh` nodig, K4), 4 INBOX_FULL. Rapporten blijven in elke volgende FETCH staan tot `rapporten_ok` ze bevestigt (I6). Na een sync stuurt Alice meteen een FETCH met het SYNCED-rapport.
- **DEPOSIT status**: 0 STORED, 1 ALREADY_STORED, 0x10 NOT_AUTH, 0x11 UNKNOWN_OWNER, 0x12 QUOTA, 0x13 NO_STORAGE, 0x14 TOO_BIG, 0x15 RATE_LIMITED.
- **STATUS state**: 0 UNKNOWN, 1 STORED, 2 ON_RADIO, 3 DELIVERED, 4 REJECTED, 5 EXPIRED, 6 SYNC_EXPIRED. Alleen voor berichten die M daadwerkelijk bewaart; voor direct afgeleverde berichten gebruikt Bob `RECEIPT_QUERY` (I6). `STATUS FORGET` vervalt: M ruimt na 30 dagen op (Y2).
- **RECEIPT_QUERY state**: 0 UNKNOWN (niet ontvangen), 1 ON_RADIO, 2 SYNCED (+`ACK_S`), 3 EVICTED. Leesactie zonder bijwerking, dus geen timestamp-replaycheck (Y2). Werkt ook als goedkope probe (par. 4).
- M weigert REQ's met `timestamp < last_timestamp` per client (`examples/simple_room_server/MyMesh.cpp:541-545`). Bob en Alice persisteren hun laatst gebruikte REQ-timestamp en gebruiken `max(nu, laatste + 1)`, omdat de klok na een reboot zonder app terugvalt op de laatste `lastmod + 1` (`BaseChatMesh.cpp:60-70`, K2).

### Companion protocol (fork)

| code | inhoud |
|---|---|
| `CMD_RDM_ENABLE` | client meldt zich aan voor 0x91 (per verbinding) |
| `PUSH_CODE_RDM_STATUS` 0x91 | `app_ack(4) | state(1) | K(4) | timestamp(4)`; states QUEUED, CUSTODY, ON_RADIO, DELIVERED, ON_RADIO_FINAL, REJECTED, EXPIRED, SYNC_EXPIRED, TOO_BIG, NO_OUTBOX |
| `CMD_RDM_SET_MAILBOX` | Alice: mailbox pubkey(32), `K_owner`(16); leeg = geen mailbox |
| `CMD_RDM_LIST_OUTBOX` | outbox-entries met state |

M staat niet in `contacts[]` maar in een verborgen peer-tabel van de fork (max 4), zodat hij niet in de app verschijnt en niet door een volle contactlijst wordt overschreven (K3). Statuswijzigingen terwijl de app weg is blijven als vlag `unreported` bewaard.

## 4. Toestandsmachines

**Afzender (Bob, per outbox-entry)**

```
NEW --verstuurd--> WAIT_ACK
WAIT_ACK --ACK_R met cap--> ON_RADIO ; --ACK_R zonder cap--> ON_RADIO_FINAL (klaar)
WAIT_ACK --timeout, Alice heeft MBX, tekst <=152--> DEPOSITING
WAIT_ACK --timeout, anders--> RETRY
RETRY --probe UNKNOWN/EVICTED, backoff (Alice zonder bekende cap), of Alice gehoord--> WAIT_ACK
DEPOSITING --STORED/ALREADY_STORED--> CUSTODY ; --NOT_AUTH--> REGISTER --ok--> DEPOSITING (max 1x)
DEPOSITING --QUOTA/NO_STORAGE/geen antwoord--> RETRY
CUSTODY --STATUS ON_RADIO met geldige ACK_R, of ACK_R direct--> ON_RADIO
CUSTODY --probe naar Alice geeft UNKNOWN--> directe DM, blijft CUSTODY (I4)
CUSTODY --STATUS UNKNOWN--> RETRY ; --STATUS REJECTED--> REJECTED
ON_RADIO --ACK_S, SYNCED of EVICTED van Alice, STATUS DELIVERED met geldige ACK_S--> DELIVERED
ON_RADIO --RECEIPT_QUERY UNKNOWN--> RETRY (Alice is het kwijt)
ON_RADIO --T_sync--> SYNC_EXPIRED (ACK_S 90 d in receipt-watch)
WAIT_ACK/RETRY/CUSTODY --T_radio--> EXPIRED
outbox vol --> gewone DM zonder entry, status NO_OUTBOX (K6)
```

Een foute ack in een antwoord laat de state ongewijzigd en wordt gelogd.

**Probes (I10).** Alice' companion zendt uit zichzelf niets, dus "Alice gehoord" is een zwakke trigger. Bob stuurt daarom voor een contact met bekende cap een gebundelde `RECEIPT_QUERY` als probe (0,41 s in plaats van 1,72 s voor de DM). Antwoord UNKNOWN of EVICTED betekent: Alice is bereikbaar en heeft het niet; Bob stuurt de DM meteen, direct over het pad uit het antwoord. Voor contacten zonder bekende cap blijven hele DM's met backoff (standaard ontvangers antwoorden niet op onbekende req_types). Een periodieke advert van Alice is verworpen: zero-hop bereikt alleen buren, flood kost bij elke repeater zendtijd.

**Mailbox (per bericht)**

```
DEPOSIT + Pi bevestigt commit --> STORED --in FETCH-antwoord--> SENT
SENT --volgende FETCH zonder rapport, of INBOX_FULL--> STORED (volgende FETCH krijgt hem opnieuw, nieuwe tag)
SENT --rapport ON_RADIO/DUPLICATE--> ON_RADIO (ciphertext blijft)
SENT --rapport UNDECRYPTABLE--> REJECTED
ON_RADIO --rapport SYNCED--> DELIVERED (ciphertext gewist, ACK_S bewaard)
ON_RADIO --FETCH met RESYNC--> STORED
STORED/SENT --T_radio--> EXPIRED ; ON_RADIO --T_sync--> SYNC_EXPIRED
DELIVERED/REJECTED/*EXPIRED --30 d--> (weg)
```

**Ontvanger (Alice)**

```
DM ontvangen (direct, flood of uit FETCH-antwoord) -> ontsleuteld -> register?
  bekend -> ACK_R opnieuw, plus ACK_S als gesynct; niet tonen (kopie van M: rapport DUPLICATE)
  nieuw  -> inbox (quotum per afzender) -> register -> ACK_R naar Bob (kopie van M: rapport ON_RADIO)
            inbox vol of schrijven mislukt -> geen ACK (kopie van M: rapport INBOX_FULL)
app synct -> register SYNCED -> ACK_S (6 bytes, alleen bij cap), kopie van M: FETCH met SYNCED-rapport
FETCH: bij boot (+0-60 s jitter), elke 60 min, na een sync, bij advert van M (min 10 min),
       en direct opnieuw zolang 'resterend' > 0 en de inbox ruimte heeft
```

Alice verwijdert nooit zelf een ongesynct bericht.

## 5. Zendtijd, rate-limits, TTL, autorisatie

Preamble is 32 symbolen bij SF ≤ 8 (`src/helpers/radiolib/RadioLibWrappers.h:56`). Berekend met de Semtech-formule (SF8, BW 62,5 kHz, CR 4/8, CRC, expliciete header, LDRO uit), per hop, raw = header + path_len + 1 pad-byte + payload:

| pakket | raw bytes | s |
|---|---|---|
| fork-DM 157 tekens, DEPOSIT 152 tekens, FETCH-antwoord met kopie | 183 | 1,72 |
| fork-DM 100 tekens | 119 | 1,20 |
| DEPOSIT-antwoord, FETCH met 1 rapport | 39 | 0,54 |
| FETCH zonder rapport, antwoord zonder kopie, STATUS, `RECEIPT_QUERY` en antwoorden (n=1) | 23 | 0,41 |
| `ACK_R` met cap (7), `ACK_S` (6) | 10 / 9 | 0,28 |

| scenario | pakketten | s per hop |
|---|---|---|
| mailboxbericht: DEPOSIT+antw, FETCH+antw met kopie, FETCH(ON_RADIO)+antw, FETCH(SYNCED)+antw, 2× STATUS+antw | 12 | 7,9 |
| zonder mailbox, telefoon een dag weg: DM, `ACK_R`, 3× `RECEIPT_QUERY`+antw, `ACK_S` | 9 | 4,7 |
| leeg ophalen per uur | 2 | 0,8 |

Toets tegen de 20%-norm van stap 9: de vorige versie gaf 6,9 s per mailboxbericht; dezelfde (oude) flow kost met preamble 32 en alle pakketten 8,5 s (`04` I2): 23% boven de voorspelling, dus boven de norm. De huidige getallen zijn per pakket gerekend en gelden als referentie. De flood-`ACK_R` van Alice na een mailboxkopie is vervallen (Y1).

| wie | limiet |
|---|---|
| Bob, Alice met bekende cap | probe direct elke 30 min het eerste etmaal, daarna elke 2 u; flood-probe hooguit elke 4 u; gebundeld per contact |
| Bob, Alice zonder bekende cap | DM-backoff 5 min, 15 min, 1 u, 4 u, 12 u, daarna elke 24 u; hooguit 1 flood per contact per 4 u |
| Bob, globaal | 1 outbox-zending per 60 s, alleen bij lege TX-queue |
| Bob in ON_RADIO | `RECEIPT_QUERY` na 1 u, 4 u, 12 u, daarna elke 24 u, plus bij horen van Alice (min 1 u) |
| Bob in CUSTODY | STATUS naar M: 10 min, 30 min, 1 u, 4 u, daarna elke 12 u, plus bij advert van M; probe naar Alice elke 24 u en bij horen van Alice (min 1 u) |
| M per client | 1 REQ per 5 s, 60 per uur; ANON_REQ via een globale limiter zoals `anon_limiter` in PR #3078 |
| T_radio / T_sync | 7 dagen (max 30, per owner op de Pi) / 30 dagen; later `ACK_S` zet via receipt-watch alsnog DELIVERED |
| Autorisatie | owners alleen via de Pi; depositors met geldig `token_B` (HMAC-check op de Pi), quotum 20 berichten per depositor, denylist per pubkey; FETCH alleen van de owner; `RECEIPT_QUERY` alleen van contacten, voor eigen berichten |

## 6. Persistente opslag (I9)

Op 4 MB-ESP32-borden met `min_spiffs.csv` is SPIFFS 128 KB (partitie 0x20000), gedeeld met contacten en prefs; een SPIFFS-bestand kost minimaal twee pagina's van 256 B. Daarom geen bestand per slot, maar per structuur één bestand met vaste records (volgnummer + CRC). Statusrecords (outbox, register) in A/B-paren: een afgebroken schrijfactie laat de andere kopie geldig. Inbox-records worden eenmaal geschreven en bij sync vrijgegeven; een afgebroken vrijgave geeft hooguit een heraanbieding. Records worden in-place bijgewerkt (seek + write), niet via `openWrite`, dat op nRF52 eerst verwijdert (`DataStore.cpp:34-37`). Aantallen worden bij boot afgeleid uit `SPIFFS.totalBytes()`/`getStorageTotalKb` (`DataStore.cpp:105-127`); onder een minimum schakelt de fork RDM uit.

| structuur | record | ESP32 8 MB | ESP32 4 MB (streef) | nRF52 zonder extra FS |
|---|---|---|---|---|
| Outbox: pubkey 6, timestamp 4, K 4, klasse/attempt/state/vlaggen 4, `app_ack` 4, pkt_hash 8, T_radio/T_sync 8, tekst 1+157 | 196 × 2 | 16 | 8 | 8 |
| Receipt-watch: `ACK_S` 6, verloop 4 | 10 | 128 | 64 | 32 |
| Register: sender 4, timestamp 4, K 4, `ACK_R` 4, `ACK_S` 6, state 1, pkt_hash-prefix 4, rapport-vlag 1 | 28 × 2 | 512 | 256 | 64 |
| Inbox: frame tot de app | 176 | 256 | ~100 | 24 |
| Per contact: cap 1, watermerk 4, mailbox-pubkey 32, token 8 | 45 | 16 | 16 | 8 |
| Totaal | | ~82 KB | ~38 KB | ~12 KB |

- Inboxquotum per afzender: hooguit een kwart van de inbox (K8), ook voor standaard afzenders.
- nRF52-cijfers gaan uit van 28 KB interne LittleFS (framework-documentatie, lokaal niet geverifieerd); seek-en-schrijf op Adafruit LittleFS wordt in stap 2 geverifieerd.

## 7. Dedicated mailbox: Heltec en eigen Pi (beslissing 3)

Eigen Heltec V3 (868) met `examples/dm_mailbox`, eigen Pi met `mbxd`. `meshcore-room/pi/rs01d.py` is codebasis: seriële poort één keer openen en vasthouden (elke open reset de ESP32), SQLite, systemd-unit. Geen pty-doorgifte naar MQTT, geen webpagina (R4, AUP). De radio doet crypto, peer-cache en zendplanning; `mbxd` is bron van waarheid voor berichten, ACL, owners, tokens.

| richting | regel (115200 baud) |
|---|---|
| radio -> Pi | `@MBX STORE <id> <owner4> <sender32hex> <hash8hex> <b64 payload>`, `@MBX REG <id> <sender32hex> <owner4> <token8hex>`, `@MBX FETCH <id> <owner4> <flags> <rapporten>`, `@MBX STAT <id> <sender> <hash>...` |
| Pi -> radio | `mbx.ok <id>` na `COMMIT` met `synchronous=FULL`, `mbx.err <id> <code>`, `mbx.fetch <id> <resterend> <ok> [<b64>]`, `mbx.stat <id> <state:ack,...>`, `mbx.acl <pub> <owner>` |

- DEPOSIT wordt pas STORED na `mbx.ok`; geen antwoord binnen 3 s geeft NO_STORAGE. Zonder Pi geen deposits, geen RAM-fallback.
- De Pi controleert `token_B` met `K_owner` en de denylist; de radio kent `K_owner` niet.
- De radio houdt de pubkeys en secrets van actieve depositors in een RAM-cache (64 × 64 B); een cache-miss laat Bob na een time-out opnieuw registreren.
- CLI-regelbuffer onder `WITH_DM_MAILBOX` naar 384 bytes (nu 152, toelichting bij `MAX_POST_BYTES` in `rs01d.py`).
- Schema: `messages(pkt_hash PK, owner, sender, payload, created, t_radio, t_sync, state, ack_r, ack_s)`, `acl(pubkey, owner, sent_count, created)`, `owners(pubkey, k_owner, ttl_days, sync_days, quota)`, `denylist(owner, pubkey)`.

## 8. Build-flags en compatibiliteit

- `WITH_RELIABLE_DM` (companion) en `WITH_DM_MAILBOX` (mailbox): alle nieuwe logica plus de exacte lengtecheck in `Mesh::createDatagram`. Zonder flags bit-identiek aan upstream.
- `RDM_STATUS_CHANNEL` (companion, optioneel, na stap 3).
- Standaard repeaters forwarden alles zoals nu (payloads ≤ 180 bytes). Standaard companions: fork-DM's worden getoond en geACKt, `RDM_CTRL` en onbekende req_types stil genegeerd, onbekende `ACK_S` genegeerd.
- Wat M ziet: pubkeys van Bob en Alice, grootte, tijdstippen en wanneer Alice synct. Niet de inhoud (R3).

## 9. Beslissingen en open punten

Besloten door Andy op 29 sep 2026:

1. **Zichtbaarheid "in bewaring"**: `PUSH_CODE_RDM_STATUS` (0x91) en `CMD_RDM_*`, plus optioneel statuskanaal "rdm-status" achter `RDM_STATUS_CHANNEL`.
2. **"Afgeleverd" = Alice' telefoon heeft het opgehaald**: `ACK_R` voor "op Alice' radio", `ACK_S` bij sync (par. 1a, 1c).
3. **Dedicated mailbox** met eigen Heltec en eigen Pi (`mbxd`), rs01d als codebasis.
4. **Autorisatie**: token van Alice via MBX_INFO, quotum per afzender; uitgewerkt als token per afzender (HMAC) en alleen naar favorieten (review I3).
5. **Upstream**: bouwen met voorlopige nummers; GitHub-discussie pas met meetdata; #3518-fix als losse PR-kandidaat, nog niet indienen.

6. **`SEND_CONFIRMED` (vinkje "delivered" in de standaard app) bij `ACK_R`** (uit review I8). Het vinkje betekent "op Alice' radio", zoals nu; de app stopt met retryen en de gebruiker stuurt niet handmatig opnieuw. "Afgeleverd" (telefoon) blijft volgens beslissing 2 in protocol en outbox, zichtbaar via 0x91 of het statuskanaal. Geaccepteerd nadeel: in de standaard app zonder statuskanaal ziet Bob het verschil tussen radio en telefoon niet. Verworpen: `SEND_CONFIRMED` pas bij `ACK_S`, omdat de standaard app dan "failed" toont tot Alice synct en handmatige herzendingen duplicaten geven die Alice' dedup niet vangt.

Open punten (meting in stap 3):

- Lust de standaard app `CMD_SYNC_NEXT_MESSAGE` tot `NO_MORE_MESSAGES`? Zo niet, dan telt het laatste bericht pas bij de volgende verbinding als gesynct.
- Zet een late `SEND_CONFIRMED` na de app-timeout de status nog op "delivered"? Relevant als `ACK_R` pas via de outbox binnenkomt, nadat de app zelf al "failed" toonde.
- Ontdubbelt de standaard app een opnieuw aangeboden bericht na een afgebroken sync?

## 10. Implementatieplan

Unit tests met `pio test -e native` (googletest, `platformio.ini:163-170`, mocks in `test/mocks`). Logica in losse klassen (`RdmOutbox`, `RdmRegister`, `RdmInbox`, `RdmCodec`, `RdmRecordFile`) met geïnjecteerde klok en opslag. HIL: twee companions (Bob, Alice) op 869.618/62.5/SF8, vanaf stap 6 plus de dedicated mailbox (Heltec + Pi).

| stap | inhoud | test |
|---|---|---|
| 1 | #3518: geen ACK als `queueMessage` faalt (PR-kandidaat, niet indienen) | Unit: volle queue geeft geen ACK. HIL: Alice' app los, queue vol, Bob krijgt geen confirm; na sync en retry wel. |
| 2 | `RdmRecordFile` (vaste records, CRC, A/B), persistente inbox met quotum, register met watermerk, `ACK_R` na opslag, exacte lengtecheck | Unit: torn-write-herstel, afleiden van aantallen uit FS-grootte, sleutel `K` gelijk over attempt-klassen, EVICTED alleen onder watermerk, lengtegrenzen 157/152/136. HIL: reboot van Alice' radio met ongesyncte berichten; retry met ander attempt 1x getoond; op een 4 MB-bord (T-Beam) vrije ruimte na vullen. |
| 3 | Cap-trailer en cap-ACK, sync-detectie, `ACK_S`, `RECEIPT_QUERY` | Unit: testvectoren `ACK_S` en `K` met twee attempt-klassen; standaard ontvanger-pad negeert trailer (bestaande ACK ongewijzigd); PATH-extra met nulpadding geeft geen cap. HIL met standaard app: de drie open punten uit par. 9; `SEND_CONFIRMED` bij `ACK_R` (beslissing 6). |
| 4 | Outbox (optie A): app-retries op één entry, firmware-attempts vanaf 252, probes, T_radio/T_sync, receipt-watch, `unreported`, NO_OUTBOX, REQ-timestamp persistent | Unit: toestandsmachine met nepklok, inclusief I8 (a)-(c), EVICTED in ON_RADIO en ervoor, UNKNOWN in ON_RADIO. HIL: Alice' radio 3 u per dag aan (timer), tijd tot aflevering meten; Bob reboot tussendoor. Meetpunt voor het kantelpunt. |
| 5 | MBX_INFO met `token_B`, alleen naar favorieten; verborgen peer-tabel | Unit: HMAC-testvector, codec. HIL: fork en standaard in alle vier combinaties; auto-added contact krijgt geen MBX_INFO. |
| 6 | `examples/dm_mailbox` + `mbxd` | Unit: codecs DEPOSIT/FETCH/STATUS; pytest `mbxd` (commit voor `mbx.ok`, token/denylist, quota, T_radio/T_sync, RESYNC, SENT zonder rapport terug naar STORED, herstart). HIL: registratie met geldig, ongeldig en ingetrokken token; deposit met Pi aan en uit. |
| 7 | Mailbox-client: deposit, STATUS, FETCH met rapporten, probes in CUSTODY | HIL: Alice' radio uit, Bob deponeert en gaat uit; Alice' radio aan (kopie, ON_RADIO); app verbindt (SYNCED); Bob aan (DELIVERED). Negatief: `mbxd` met willekeurige ack, `ACK_R` als `ACK_S` aangeboden, Alice kent Bob niet (REJECTED), mailbox houdt achter terwijl Alice bereikbaar is (probe levert direct af, I4), FETCH-antwoord verloren (heraflevering, 1x getoond), 153 bytes tekst (TOO_BIG). |
| 8 | App-zichtbaarheid volgens beslissingen 1 en 6; statuskanaal na stap 3 | meshcore_py-script met `CMD_RDM_ENABLE` en 0x91; standaard app zonder aanmelding krijgt geen 0x91; statuskanaal bij volle queue. |
| 9 | Zendtijd meten per scenario (tellingen op de mailbox-Pi en de companions) en PR's voorbereiden | Meting naast par. 5; afwijking > 20% verklaren. Daarna GitHub-discussie over de voorlopige nummers. |
