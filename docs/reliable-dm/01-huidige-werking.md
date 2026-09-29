# 01: Hoe DM's nu werken en waar ze verloren gaan

Basis: branch `feature/reliable-dm` op `5d266dcb` (upstream/dev). Alle verwijzingen zijn `pad:regel` in deze repo.
"Bob" is de afzender, "Alice" de ontvanger. "Radio" is de companion-node, "app" de telefoon-app via het companion protocol.

## 1. De keten in het kort

1. App stuurt `CMD_SEND_TXT_MSG` (txt_type, attempt, timestamp, 6-byte pubkey-prefix, tekst) naar Bobs radio (`examples/companion_radio/MyMesh.cpp:1132-1177`).
2. Radio bouwt het TXT_MSG-pakket (`src/helpers/BaseChatMesh.cpp:451-471`) en verstuurt direct als er een `out_path` is, anders flood (`BaseChatMesh.cpp:473-490`).
3. Radio antwoordt de app met `RESP_CODE_SENT` (flood-vlag, `expected_ack`, `est_timeout`) en zet de ACK in een ringtabel van 8 (`MyMesh.cpp:1160-1171`, `MyMesh.h:280-287`).
4. Alice' radio ontvangt, ontsleutelt, zet het bericht in de offline queue en stuurt meteen een ACK (`BaseChatMesh.cpp:231-257`, `MyMesh.cpp:440-479`).
5. Komt de ACK aan bij Bobs radio, dan volgt `PUSH_CODE_SEND_CONFIRMED` (0x82) met ack en trip time (`MyMesh.cpp:417-438`).
6. Komt er niets, dan gebeurt er in de firmware niets: `onSendTimeout()` is leeg (`MyMesh.cpp:898`). "Failed" is een conclusie van de app op basis van `est_timeout`.

## 2. Pakket- en payloadformaat

Header `0bVVPPPPRR` (`src/Packet.h:8-12`): 2 bits versie, 4 bits payload type, 2 bits route type.

TXT_MSG-payload (`src/Mesh.cpp:488-510`, `docs/payloads.md:197-212`):

| veld | bytes | bron |
|---|---|---|
| dest_hash | 1 | `Identity::copyHashTo`, eerste byte van pubkey (`src/Identity.h:19-22`, `PATH_HASH_SIZE=1` in `src/MeshCore.h:18`) |
| src_hash | 1 | idem, van de afzender |
| MAC | 2 | `CIPHER_MAC_SIZE=2` (`src/MeshCore.h:17`) |
| ciphertext | n*16 | AES-128 over `timestamp(4) | flags(1) | tekst [| 0x00 | attempt]` |

- `flags = (txt_type << 2) | (attempt & 3)` (`BaseChatMesh.cpp:458`, `:498`). Bij `attempt > 3` worden `0x00` en het volle attempt-byte na de tekst geplakt (`BaseChatMesh.cpp:465-468`).
- De dest/src-hash in de payload is altijd 1 byte. `path.hash.mode` (1..3 bytes) geldt alleen voor het `path`-veld (`Packet.h:79-83`, `MyMesh.cpp:493`), niet voor de payload-hashes (`Mesh.cpp:135-136`).
- Max tekst 160 bytes (`BaseChatMesh.h:8`), max payload 184 (`MeshCore.h:20`).

## 3. Crypto

- Shared secret: Ed25519-sleutels omgezet naar X25519, ECDH (`src/Identity.cpp:155-157`), 32 bytes, per contact gecachet (`src/helpers/ContactInfo.h:21-27`). Statisch per paar: geen forward secrecy, geen sessiesleutel.
- Encryptie: AES-128 in ECB-modus met de eerste 16 bytes van het secret, zero padding, geen IV (`src/Utils.cpp:107-124`). De timestamp in het eerste blok maakt ciphertexts uniek.
- MAC: HMAC-SHA256 met het volle 32-byte secret over de ciphertext, afgekapt tot 2 bytes (`Utils.cpp:127-145`, check `:147-175`).
- ACK-waarde: eerste 4 bytes van `SHA256(timestamp | flags | tekst, pubkey_van_afzender)`. Bob berekent hem vooraf (`BaseChatMesh.cpp:462`), Alice na ontsleutelen (`BaseChatMesh.cpp:244-245`). Byte 5 is het extended attempt-byte, byte 6 random, alleen om de pakket-hash uniek te maken (`BaseChatMesh.cpp:247-248`).
- Gevolg: alleen wie de plaintext kent (Bob, Alice) kan de ACK-waarde berekenen. Een derde die alleen het pakket heeft kan dat niet. Wel is de ACK zelf onversleuteld zodra hij als los ACK-pakket gaat (`Mesh.cpp:560-572`).

## 4. Routing, ACK-pad en path-reset

- Flood: elk forwardend knooppunt voegt zijn hash toe aan `path` (`Mesh.cpp:344-357`). Alice antwoordt op een flood-DM met een PATH-pakket (flood) waarin het pad Bob->Alice en de ACK versleuteld als "extra" zitten (`BaseChatMesh.cpp:250-254`, `Mesh.cpp:448-486`). Bob slaat dat pad op als `out_path` (`BaseChatMesh.cpp:353-370`) en stuurt Alice op zijn beurt het omgekeerde pad direct (`Mesh.cpp:173-178`).
- Direct: Alice stuurt een los ACK-pakket over haar `out_path` naar Bob, of flood als ze geen pad heeft (`BaseChatMesh.cpp:43-58`). Optioneel extra multi-ACKs (`MyMesh.cpp:287-289`, `Mesh.cpp:379-402`).
- Tussennodes op het directe pad verwerken een ACK al "vroeg" (`Mesh.cpp:78-87`).
- `out_path` wordt alleen vervangen, nooit gevalideerd (`BaseChatMesh.cpp:354-356`). Reset gebeurt alleen op verzoek van de app via `CMD_RESET_PATH` (`MyMesh.cpp:1317-1327`) of `resetPathTo` (`BaseChatMesh.cpp:834-836`). De firmware doet zelf geen flood-fallback na een mislukte direct send.
- Timeouts: flood `500 + 16 * airtime`, direct `500 + (6 * airtime + 250) * (hops + 1)` ms (`MyMesh.cpp:108-111`, `:888-896`). `BaseChatMesh` houdt maar een enkele `txt_send_timeout` bij (`BaseChatMesh.h:68`, `BaseChatMesh.cpp:986-993`).
- Retries: de firmware retryt niet. Het `attempt`-byte komt uit de app (`MyMesh.cpp:1135`). Hoeveel pogingen en wanneer de app `CMD_RESET_PATH` + flood doet, staat in de (closed source) app, niet in deze repo.

## 5. Dedup en replay

- Pakket-hash = eerste 8 bytes van `SHA256(payload_type | payload)`. Header, route type en `path` tellen niet mee (behalve bij TRACE) (`src/Packet.cpp:41-50`).
- `SimpleMeshTables`: ringbuffer van 160 hashes in RAM (`src/helpers/SimpleMeshTables.h:9-57`). Companion en repeater gebruiken dezelfde klasse (`examples/companion_radio/main.cpp:116`, `examples/simple_repeater/main.cpp:17`). `restoreFrom/saveTo` bestaat alleen voor ESP32 en wordt door de companion niet aangeroepen: na reboot is de tabel leeg.
- Eerste kopie wint; latere kopieen met dezelfde hash worden niet verwerkt en niet opnieuw geACKt (`Mesh.cpp:141-143`).
- Bij het zelf verzenden markeert de afzender het pakket als gezien (`Mesh.cpp:651`, `:713`).
- Timestamp-replaybescherming bestaat alleen voor adverts (`BaseChatMesh.cpp:133-136`). Voor `TXT_TYPE_PLAIN` wordt de sender timestamp niet gecontroleerd; `TXT_TYPE_SIGNED_PLAIN` werkt alleen `sync_since` bij en weigert niets (`BaseChatMesh.cpp:291-293`). Een oud pakket dat buiten het 160-venster opnieuw binnenkomt wordt dus gewoon opnieuw getoond en geACKt.

## 6. Status naar de app

| status | mechanisme | bron |
|---|---|---|
| sent | `RESP_CODE_SENT` met flood-vlag, `expected_ack`, `est_timeout` | `MyMesh.cpp:1167-1171` |
| delivered | `PUSH_CODE_SEND_CONFIRMED` (0x82): ack(4) + trip_time(4) | `MyMesh.cpp:426-430` |
| failed | geen frame; app beslist na `est_timeout` | `MyMesh.cpp:898` |
| send error | `ERR_CODE_TABLE_FULL` als het pakket niet gebouwd kan worden (pool leeg, te lang) | `MyMesh.cpp:1157-1158` |

"Delivered" betekent: Alice' radio heeft het ontsleuteld en in RAM gezet. Het zegt niets over Alice' app of Alice zelf.

## 7. Antwoord 1: alle manieren waarop een DM nu verloren gaat

Twee betekenissen van "offline" lopen door elkaar: (a) Alice' app is weg maar haar radio draait, (b) Alice' radio zelf is uit of buiten bereik.

| # | scenario | wat er gebeurt | Bob ziet | bron |
|---|---|---|---|---|
| L1 | Alice' radio uit / buiten bereik (b) | Niemand bewaart het pakket. Bobs app doet een paar pogingen binnen seconden en stopt. Geen outbox in de firmware. | failed | `MyMesh.cpp:898`, `BaseChatMesh.cpp:986-993` |
| L2 | Alice' app weg, radio aan (a), queue vol (#3518) | Bericht wordt ontsleuteld, `queueMessage` probeert de queue (RAM; 16 frames in de default, `MyMesh.h:61-62`, maar 256 op o.a. Heltec V3 BLE, `variants/heltec_v3/platformio.ini:170`; correctie na `07-simulator.md`). Is die vol en zit er geen kanaalbericht in om te verdringen, dan wordt het nieuwe DM stil weggegooid. De ACK gaat daarna toch de lucht in. | delivered (onterecht) | `MyMesh.h:61-62`, `MyMesh.cpp:224-241`, ACK na queue in `BaseChatMesh.cpp:241-256` |
| L3 | Alice' app weg, radio reboot/batterij leeg | Offline queue staat alleen in RAM. Alles wat al geACKt was is weg. | delivered (onterecht) | `MyMesh.h:277-278`, `MyMesh.cpp:906` |
| L4 | Bobs app weg of telefoon niet verbonden | Er wordt niets verzonden: alleen de app initieert een DM, de radio heeft geen outbox. Pending-state leeft in de app. | n.v.t. | `MyMesh.cpp:1132` |
| L5 | Retries op | Retries en flood-fallback zijn app-logica. Na de laatste timeout is het bericht "failed" en moet de gebruiker het handmatig opnieuw sturen. | failed | `MyMesh.cpp:1135`, `:888-896` |
| L6 | ACK verloren, bericht wel aangekomen | ACK is fire-and-forget (optioneel multi-ACK). Bob ziet failed; een retry met ander attempt-byte heeft een andere pakket-hash en wordt bij Alice als nieuw bericht getoond (duplicaat). | failed + duplicaat bij Alice | `BaseChatMesh.cpp:43-58`, `:458`, `Packet.cpp:41-50` |
| L7 | Pad kapot | Direct send over een verouderde `out_path`: de eerste hop bestaat niet meer of hoort Bob niet, niemand forwardt. Alleen `CMD_RESET_PATH` vanuit de app herstelt dit. | failed tot app reset | `BaseChatMesh.cpp:484-487`, `MyMesh.cpp:1317-1327` |
| L8 | Pad vergiftigd | Elk ontvangen PATH-pakket overschrijft `out_path` zonder check. | failed bij volgende direct send | `BaseChatMesh.cpp:354-356` |
| L9 | Dedup: identieke herhaling | Stuurt de app een retry met dezelfde timestamp en hetzelfde attempt, dan is de hash gelijk. Repeaters en Alice laten hem vallen; Alice ACKt niet opnieuw. | failed | `Mesh.cpp:141`, `Packet.cpp:41-50` |
| L10 | Alice kent Bob niet als contact | Alice kan niet ontsleutelen (geen secret), pakket genegeerd. Contactlijst vol of auto-add uit maakt dit waarschijnlijker. | failed | `Mesh.cpp:147-191`, `BaseChatMesh.cpp:157-182` |
| L11 | Resources | Pakketpool van 16: bij een lege pool wordt een ontvangen pakket gedropt of kan niet verzonden worden. Duty-cycle budget kan TX voorbij `est_timeout` uitstellen. | failed / table full | `MyMesh.cpp:901`, `src/Dispatcher.cpp:201-203`, `:281-284` |
| L12 | Scope/regio | Flood gaat standaard met `default_scope_key` als transport code; repeaters met regiofilter kunnen hem weigeren. | failed | `MyMesh.cpp:502-512` |
| L13 | Tweede DM tijdens lopende timeout | `txt_send_timeout` is enkelvoudig en de ACK-tabel heeft 8 plekken; bij veel parallelle berichten vallen oude verwachte ACKs uit de ring, een late ACK levert dan geen `SEND_CONFIRMED` meer op. | failed (onterecht) | `BaseChatMesh.h:68`, `MyMesh.h:285` |

Kern: er is nergens persistente opslag van een DM (niet bij Bob, niet onderweg, niet bij Alice na de ACK), en de ACK betekent "radio heeft het in RAM", niet "de app heeft het".

## 8. Antwoord 2: herbruikbare bouwstenen voor een door een derde bewaarde DM

### Kan een derde (mailbox M) een TXT_MSG bewaren en later ongewijzigd uitzenden?

Ja, technisch wel. De payload (dest_hash, src_hash, MAC, ciphertext) is onafhankelijk van route type en `path`. M kan hem opslaan (`Packet::writeTo/readFrom`, `src/Packet.cpp:52-85`) en later met een nieuwe header opnieuw verzenden via `sendFlood` of `sendDirect` (`Mesh.cpp:637-715`). Alice zoekt via src_hash haar contact Bob, rekent ECDH(Alice, Bob) en de MAC klopt (`Mesh.cpp:147-185`). M kan de inhoud niet lezen, omdat M geen van beide privesleutels heeft. Voorwaarde: Bob staat in Alice' contacten.

### Wat breekt er

| aspect | probleem | bron |
|---|---|---|
| Dedup onderweg | De hash negeert path en route. Repeaters die het origineel zagen droppen een replay zolang de hash in hun 160-ring zit. Bij uren later is dat meestal geen probleem, direct erna wel. | `Packet.cpp:41-50`, `SimpleMeshTables.h:34-57` |
| Dedup bij Alice | Kreeg Alice het origineel al en is de hash nog in haar ring: replay wordt stil gedropt (goed). Buiten het venster of na reboot: tweede keer getoond en geACKt (duplicaat). Er is geen persistente "al afgeleverd"-set. | idem, `BaseChatMesh.cpp:239-257` |
| Path bij flood-replay | Alice antwoordt met een PATH-pakket met het pad M->Alice. Bob ontvangt dat en zet het als zijn `out_path` naar Alice: vergiftigd pad (L8) als Bob niet naast M staat. | `BaseChatMesh.cpp:250-254`, `:354-356` |
| Path bij direct-replay | M heeft zelf een `out_path` naar Alice nodig (uit haar advert of eerder verkeer). Alice ACKt dan los over haar eigen pad naar Bob, of flood als ze geen pad heeft. | `BaseChatMesh.cpp:43-58` |
| ACK-route terug naar Bob | Bob is mogelijk offline als Alice ACKt; niemand bewaart de ACK. Is Bob wel online, dan matcht `processAck` alleen als de ACK nog in de RAM-ring van 8 staat en de radio niet gereboot is; de app heeft allang "failed" getoond. | `MyMesh.cpp:417-438`, `MyMesh.h:285` |
| ACK zichtbaar voor M? | Bij flood-replay zit de ACK versleuteld in het PATH-pakket (alleen Bob kan hem lezen). Bij direct-replay is het een los, onversleuteld ACK-pakket dat M alleen ziet als M op het retourpad zit. | `BaseChatMesh.cpp:250-256`, `Mesh.cpp:560-572` |
| Timestamp | Geen replay-check voor plain TXT, dus late aflevering breekt niet. De originele sender timestamp blijft staan, de app sorteert daarop. | `BaseChatMesh.cpp:231-241` |
| Attempt | M bewaart een concrete attempt. Bobs verwachte ACK is per attempt anders (`flags` zit in de hash). | `BaseChatMesh.cpp:458-462` |
| Metadata | M ziet 1-byte dest/src hash, lengte en tijdstip. Om Alice te bereiken moet M haar volledige identiteit kennen; dat moet Bob buiten het TXT-pakket om meegeven. | `Mesh.cpp:500-505` |

### Herbruikbare bouwstenen

- **Ongewijzigd bewaarbaar TXT_MSG**: `createDatagram` + `writeTo/readFrom` (`Mesh.cpp:488-510`, `Packet.cpp:52-85`). E2E-geheim zonder nieuwe crypto.
- **Vooraf berekenbare receipt**: de 4-byte ACK-waarde is alleen door Bob en Alice te berekenen (`BaseChatMesh.cpp:245`, `:462`). Bruikbaar als correlatie-ID. Let op: geeft Bob hem aan M, dan kan M "afgeleverd" vervalsen. Voor een echte end-to-end receipt (R5) is een door Alice versleuteld of ondertekend bewijs nodig.
- **Beveiligd kanaal naar de mailbox**: REQ/RESPONSE met eigen req_type, versleuteld met ECDH(client, M) (`BaseChatMesh.cpp:311-338`, `:663-700`), of ANON_REQ voor clients die M niet als contact hebben (`Mesh.cpp:197-224`, `:512-538`). Dit is hetzelfde patroon als room server login en requests.
- **Bereikbaarheid van Alice detecteren**: adverts (`BaseChatMesh.cpp:122-201`) en elk pakket met haar src_hash; `ConnectionInfo`/keep-alive bestaat al voor servers (`BaseChatMesh.h:47-53`, `:149-154`).
- **Opslag-hooks**: `getBlobByKey/putBlobByKey` (`BaseChatMesh.h:133-134`) en de companion `DataStore` voor persistente outbox/dedup-set aan client-zijde.
- **Room server als referentie**: per-client `sync_since`, pending ACK en push retries (`examples/simple_room_server`), maar leest inhoud en is dus alleen als patroon bruikbaar (R3).

## 9. Antwoord 3: ruimte voor een backwards-compatibele extensie

| optie | gedrag op oude nodes | bruikbaar? | bron |
|---|---|---|---|
| Header versie `VV` > 0 | Oude Dispatcher gooit het pakket direct weg, ook als repeater. Breekt RF-transport over standaard repeaters. | nee | `src/Dispatcher.cpp:153-156` |
| Payload type 0x0C-0x0E (reserved) | Direct: oude repeaters forwarden wel (generieke direct-forward gebeurt voor de switch). Flood: niet doorgestuurd. | alleen direct | `Mesh.cpp:78-108`, `:326-328`, `docs/packet_format.md:38-40` |
| `RAW_CUSTOM` 0x0F | Alleen direct, nooit flood-routed, geen crypto. | beperkt | `Mesh.cpp:292-298` |
| Nieuwe `txt_type` (4..63) in TXT_MSG | Repeaters zien gewoon TXT_MSG en forwarden flood en direct. Oude ontvanger negeert het stil en ACKt niet. | ja, voor fork-naar-fork met fallback | `BaseChatMesh.cpp:308-310`, `TxtDataHelpers.h:6-9` |
| Bytes na de NUL in een plain TXT | Precedent: extended attempt-byte (`BaseChatMesh.cpp:465-468`). Oude ontvanger toont de tekst, ACKt normaal (hash loopt tot de NUL). Eerste byte na de NUL belandt in ACK-byte 5, onschadelijk. Kost tekstruimte. | ja, volledig compatibel | `BaseChatMesh.cpp:237-248` |
| Nieuw `req_type` in REQ | Companion geeft onbekende types aan de listener, antwoordt standaard niet. Repeaters forwarden normaal. | ja, voor mailbox-protocol | `MyMesh.cpp:684-687` |
| PATH `extra_type` | Bovenste 4 bits gereserveerd; onbekende lage waarden worden genegeerd. | ja, voor receipts op het PATH-retourpad | `Mesh.cpp:170`, `BaseChatMesh.cpp:361-368` |
| Advert feature-velden `FEAT1/FEAT2` (0x20/0x40, 2 bytes) | Oude parsers slaan ze correct over. Geschikt als capability-vlag ("spreekt reliable-DM", "is mailbox"). Controleren of upstream ze niet claimt. | ja | `src/helpers/AdvertDataHelpers.h:15-16`, `AdvertDataHelpers.cpp:51-56`, `docs/payloads.md:43-57` |
| Companion protocol | `reserved1/2` in `RESP_CODE_CONTACT_MSG_RECV_V3`; push codes vanaf 0x91 vrij. Een standaard app kent alleen `SEND_CONFIRMED`, dus "in bewaring" heeft geen standaard weergave. | deels | `MyMesh.cpp:443-447`, `:117-133` |

Conclusie voor het ontwerp: versiebits en nieuwe payload types zijn onbruikbaar over standaard repeaters met flood. De veilige containers zijn bestaande versleutelde types (TXT_MSG met nieuwe txt_type of trailer, REQ/RESPONSE met nieuw req_type, PATH extra) plus een advert-feature-vlag voor capability-detectie.
