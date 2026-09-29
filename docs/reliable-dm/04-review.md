# 04: Review van het ontwerp (03 + ADR-001)

Onafhankelijke, adversariële review van `03-ontwerp.md` en `adr-001-reliable-dm.md` op branch `feature/reliable-dm` (`5d266dcb`). Beslissingen van 29 sep zijn als gegeven genomen; waar een bevinding er toch tegenaan schuurt, staat dat erbij.
Ernst: **blokkerend** (eerst oplossen, anders werkt een requirement niet), **belangrijk** (vóór de betreffende stap oplossen), **klein**.

## Samenvatting

| # | ernst | bevinding | status |
|---|---|---|---|
| B1 | blokkerend | Capability-detectie via advert FEAT1 werkt in de praktijk niet: companions adverten alleen handmatig, dus Alice stuurt vrijwel nooit `ACK_S` | verwerkt |
| B2 | blokkerend | `ACK_S` en de `RECEIPT_QUERY`-sleutel hangen af van `attempt & 3`; Bob bewaart er één, dus "afgeleverd" matcht niet na een retry in een andere klasse | verwerkt |
| I1 | belangrijk | Grens van 153 bytes klopt niet met de code: `createDatagram` weigert al boven 137 bytes tekst | verwerkt |
| I2 | belangrijk | Zendtijd gerekend met preamble 16; de code gebruikt 32 bij SF8. Mailboxflow is ~8,5 s per hop, niet 6,9 | verwerkt |
| I3 | belangrijk | Eén gedeeld token per owner plus quotum per pubkey is Sybil-baar; het token gaat automatisch naar auto-added contacten | verwerkt |
| I4 | belangrijk | Vanaf CUSTODY stopt Bob met direct afleveren; een haperende of stale mailbox maakt van "in bewaring" "verloren" | verwerkt |
| I5 | belangrijk | Een replay van dezelfde kopie door M wordt door repeaters gedropt zolang de hash in hun 160-ring staat | verwerkt |
| I6 | belangrijk | DONE SYNCED heeft geen retry, en Bob pollt met mailbox alleen M; een verloren DONE betekent nooit DELIVERED | verwerkt |
| I7 | belangrijk | Eviction van Alice' ontvangstregister geeft `RECEIPT_QUERY` UNKNOWN, Bob verstuurt opnieuw, Alice toont een duplicaat | verwerkt (aangepast) |
| I8 | belangrijk | Samenspel met de retries van de standaard app is niet gespecificeerd (dubbele outbox-entries, `SEND_CONFIRMED`-waarde, "failed" leidt tot handmatige herzending) | verwerkt (a-c) / besloten (d), beslissing 6 |
| I9 | belangrijk | Een inbox met een bestand per slot past niet op 4 MB-ESP32-borden met `min_spiffs.csv` | verwerkt |
| I10 | belangrijk | R6 leunt op "Alice gehoord", maar Alice' companion zendt uit zichzelf niets | verwerkt (ander middel) |
| Y1 | belangrijk (vereenvoudiging) | FETCH-antwoord kan de payload zelf dragen; dat schrapt replays, OFFERED/REPLAYED, de 10-minutenregel, losse DONE's en I5 | verwerkt |

Kleine punten in de laatste paragraaf.

## Blokkerend

### B1. Capability via advert komt vrijwel nooit aan

- **Claim** (`03` par. 1b, 3): Alice stuurt `ACK_S` alleen naar afzenders met FEAT1-bit `RDM`; Bob kiest ON_RADIO_FINAL als Alice het bit niet heeft; Alice stuurt MBX_INFO "de eerste keer dat ze een `RDM`-advert van een contact hoort".
- **Bewijs**: de companion maakt alleen een advert op `CMD_SEND_SELF_ADVERT` (`examples/companion_radio/MyMesh.cpp:1297`), bij export (`:1384`) of via een UI-knop (`ui-new/UITask.cpp:581`, `ui-tiny/UITask.cpp:403`); `advert()` (`MyMesh.cpp:2436`) heeft geen timer, `adv_int` is uitgecommentarieerd (`NodePrefs.h:195-196`). Veel contacten ontstaan via QR/import van een oude advert. `ContactInfo` heeft geen veld voor FEAT1 (`DataStore.cpp:302-311`), dus het bit moet apart persistent bewaard worden.
- **Gevolg**: in de normale situatie kent Alice Bobs bit niet, stuurt geen `ACK_S`, en Bob blijft op "op Alice' radio" tot T_sync. Omgekeerd krijgt Bob nooit MBX_INFO. Dat raakt R5 en de hele mailboxfase.
- **Voorstel**: capability in-band. Bob zet in elke fork-DM een trailerbyte na de NUL (precedent: extended attempt, `BaseChatMesh.cpp:465-468`; standaard ontvangers negeren hem, `01` par. 9). Alice leert zo per bericht dat Bob `RDM` spreekt en antwoordt met `ACK_S`; Alice' capability leert Bob uit het binnenkomen van `ACK_S` of een antwoord op `RECEIPT_QUERY` (standaard companions antwoorden niet, `MyMesh.cpp:684-687`, `MyMesh.h:102-103`). MBX_INFO gaat dan bij het eerste fork-bericht van een contact. FEAT1 mag blijven als hint, niet als voorwaarde. Kost 1 byte tekst (dus 157/152, zie I1).

### B2. `ACK_S` en `RECEIPT_QUERY` zijn attempt-afhankelijk

- **Claim**: `ACK_S = SHA256("RDMS" | timestamp | flags | tekst | pubkey_Bob)[0:4]`; outbox-entry bewaart één `ACK_S` (`03` par. 6); `RECEIPT_QUERY` vraagt op `ACK_R`; het register bewaart één `ACK_R`.
- **Bewijs**: het flags-byte bevat `attempt & 3` (`BaseChatMesh.cpp:457-458` bij Bob, `:245` hasht `data[4]` inclusief attemptbits bij Alice). Alice' register is attempt-onafhankelijk en bewaart de eerste ontvangen klasse; bij een duplicaat van een andere klasse (outbox-retry, mailboxkopie) is onbepaald welke `ACK_R` terugkomt.
- **Gevolg**: kreeg Alice klasse 1 en verwacht Bob `ACK_S` van klasse 0, dan matcht "afgeleverd" nooit; `RECEIPT_QUERY` met de "verkeerde" `ACK_R` geeft UNKNOWN, Bob verstuurt opnieuw (lus zonder einde tot T_radio).
- **Voorstel**: definieer `ACK_S` over `timestamp | (txt_type << 2) | tekst` (attemptbits op 0) en gebruik als `RECEIPT_QUERY`-sleutel de dedup-sleutel `SHA256(timestamp | tekst | pubkey_Bob)[0:4]`, die Bob ook kan berekenen. Testvector in stap 3 met twee klassen.

## Belangrijk

### I1. 153 bytes tekst past niet door `createDatagram`

- **Claim** (`03` par. 3): REQ-payload `4 + pad16(10 + 4 + pad16(7 + tekst)) ≤ 184` geeft 153 bytes.
- **Bewijs**: `Mesh::createDatagram` weigert als `data_len + CIPHER_MAC_SIZE + CIPHER_BLOCK_SIZE-1 > MAX_PACKET_PAYLOAD` (`src/Mesh.cpp:490`), dus plaintext ≤ 167. DEPOSIT-plaintext bij 153 tekens is `10 + 4 + 160 = 174`. Hoogste waarde die door de check komt: 137 tekens (`10 + 4 + 144 = 158`). Ook: met extended attempt is de gewone DM-grens 158, niet 160 (`BaseChatMesh.cpp:454`); dat raakt elke outbox-retry met attempt > 3.
- **Voorstel**: óf de check exact maken (`2 + CIPHER_MAC_SIZE + pad16(data_len) ≤ MAX_PACKET_PAYLOAD`, geeft ≤ 176 en dus 153; wijziging in `src/`, onder flag), óf 137 als grens documenteren. `owner_prefix` en `inner_len` schrappen helpt niet: de blokgrens van 16 domineert. Met de trailerbyte uit B1 wordt het 152 respectievelijk 136.

### I2. Zendtijd: preamble is 32 symbolen bij SF8

- **Claim** (`03` par. 5): "preamble 16", DEPOSIT 1,66 s, ACK 0,18 s, 6,9 s per mailboxbericht, 0,6 s/uur ophalen.
- **Bewijs**: `preambleLengthForSF(sf) { return sf <= 8 ? 32 : 16; }` (`src/helpers/radiolib/RadioLibWrappers.h:56`), gezet door alle wrappers (bijv. `CustomSX1262Wrapper.h:20`). Herberekend (Semtech-formule, SF8, BW 62,5 kHz, CR 4/8, CRC aan, expliciete header, LDRO uit want T_sym = 4,096 ms):

| pakket (raw bytes) | 03 | preamble 32 |
|---|---|---|
| DEPOSIT / volle DM (186) | 1,66 | 1,72 |
| DONE, DEPOSIT-antwoord (38) | 0,44 | 0,51 |
| REQ n=1 (22) | 0,31 | 0,38 |
| los ACK, direct 1 hop (2+1+6 = 9) | 0,18 (6 B) | 0,28 |

- De mailboxflow telt in `03` de twee DONE-antwoorden niet mee. Volledige flow (DEPOSIT+antwoord, FETCH+antwoord, kopie, `ACK_R`, 2× DONE+antwoord, `ACK_S`, 2× STATUS+antwoord, 15 pakketten): **8,5 s per hop** (7,5 s met preamble 16). Zonder mailbox, telefoon een dag weg: 4,5 s. Leeg ophalen: 0,76 s/uur.
- Bovendien is "per hop" alleen juist voor direct verkeer. Een mailboxkopie laat Alice een `ACK_R` naar Bob sturen (`sendAckTo`, `BaseChatMesh.cpp:43-58`): heeft Alice geen pad naar Bob, dan is dat een flood over de hele scope, per mailboxbericht.
- **Voorstel**: tabel en ADR corrigeren; de 20%-norm van stap 9 zou nu al falen. Zie Y1 voor het schrappen van de flood-`ACK_R`.

### I3. Gedeeld token en quotum per pubkey zijn Sybil-baar

- **Claim** (`03` par. 5): depositors tonen het token uit MBX_INFO ("alleen contacten van Alice krijgen het"), quotum 20 per depositor.
- **Bewijs**: één token per owner (`owners(pubkey, token, ...)`, `03` par. 7). Alice stuurt MBX_INFO automatisch aan elk `RDM`-contact; companions met auto-add maken van elke gehoorde advert een contact. Pubkeys zijn gratis; ANON_REQ-registratie draagt een zelfgekozen pubkey (`src/Mesh.cpp:512-538`).
- **Gevolg**: wie het token eenmaal heeft, maakt N sleutels × 20 deposits. Elk deposit kost M een kopie van 1,72 s per hop naar Alice plus DONE (UNDECRYPTABLE), dus zendtijdversterking richting Alice, en vult de ACL van de radio (`MAX_CLIENTS` 32, `src/helpers/ClientACL.h:44-45`).
- **Voorstel**: token per afzender afleiden, `token_B = HMAC(K_owner, pubkey_Bob)[0:8]`. Pi en Alice' radio kennen `K_owner`; M hoeft geen lijst bij te houden, een uitgelekt token werkt alleen voor die ene pubkey, intrekken per contact kan via een denylist. MBX_INFO alleen naar contacten die Alice expliciet heeft (niet auto-added).

### I4. CUSTODY zet directe aflevering stil

- **Claim** (`03` par. 4): "Vanaf CUSTODY stuurt Bob het bericht niet meer naar Alice."
- **Gevolg**: stale MBX_INFO (Alice wisselde van mailbox), een kapotte Pi of een kwaadwillende M die achterhoudt, laat het bericht na T_radio als EXPIRED eindigen, ook als Alice' radio al die tijd voor Bob bereikbaar was. Dat is slechter dan fase A en maakt van R5 ("M kan hooguit achterhouden") een verlies, terwijl R6 had kunnen afleveren.
- **Voorstel**: in CUSTODY een goedkope directe poging als Bob iets van Alice hoort (min 1 u) en een poging per 24 u. Alice' register ontdubbelt, dus dit kost alleen zendtijd. Na STATUS UNKNOWN (M kent het niet meer) terug naar RETRY.

### I5. Dezelfde kopie opnieuw uitzenden wordt onderweg gedropt

- **Claim** (`03` par. 1d, 4): M herhaalt dezelfde kopie hooguit om de 10 min; geen DONE binnen 10 min geeft STORED en een nieuwe replay.
- **Bewijs**: direct forwarden checkt `wasSeen` (`src/Mesh.cpp:101`); de ring heeft 160 plaatsen (`SimpleMeshTables.h:9-57`). M kan de ciphertext niet wijzigen, dus de hash blijft gelijk. Op een rustig kanaal zijn 160 pakketten niet binnen 10 min vol.
- **Gevolg**: de tweede tot vijfde replay komen niet aan zolang de eerste nog in de ringen van de repeaters staat; "max 5 replays" is dan grotendeels schijnwerk.
- **Voorstel**: Y1 (kopie in een RESPONSE, elke keer een unieke hash).

### I6. DONE SYNCED en STATUS-routing

- **Claim**: Alice stuurt DONE SYNCED naar M (`03` par. 4); "Met mailbox: STATUS naar M in plaats van naar Alice" (par. 5).
- **Gevolg**: DONE heeft geen retry of persistente vlag. Gaat hij verloren terwijl Bob offline is (juist het mailbox-scenario), dan blijft M op ON_RADIO en Bob ook, tot SYNC_EXPIRED. Verder: een bericht dat Bob direct afleverde staat niet bij M, dus STATUS geeft UNKNOWN; voor die berichten moet Bob `RECEIPT_QUERY` blijven gebruiken.
- **Voorstel**: "DONE pending" als registerstate bij Alice, meegestuurd met elke volgende FETCH tot M bevestigt (Y1). STATUS alleen voor entries die daadwerkelijk STORED zijn; anders `RECEIPT_QUERY`.

### I7. Register-eviction veroorzaakt duplicaten

- **Claim** (`03` par. 3): `RECEIPT_QUERY` UNKNOWN betekent "Alice heeft het niet", Bob verstuurt opnieuw. Register: 256 (ESP32) of 64 (nRF52) entries.
- **Gevolg**: Alice synct, `ACK_S` gaat verloren, daarna 64 andere DM's; Bobs volgende query (tot 30 dagen) geeft UNKNOWN, Bob verstuurt, Alice kent het niet meer en toont het opnieuw.
- **Voorstel**: per afzender een persistent laagwatermerk (oudste nog gedekte sender-timestamp). Query voor een oudere timestamp geeft EVICTED; Bob stopt dan zonder herzending. Evict alleen gesyncte entries.

### I8. Outbox tegenover de retries van de standaard app

- **Bewijs**: de app kiest attempt en timestamp (`MyMesh.cpp:1135`), matcht `PUSH_CODE_SEND_CONFIRMED` op de ack-waarde die hij in `RESP_CODE_SENT` kreeg (`MyMesh.cpp:417-434`), en retryt zelf (`01` par. 4).
- **Open in 03**: (a) een app-retry (zelfde timestamp en tekst, ander attempt) moet op de bestaande outbox-entry landen, anders dubbele entries en dubbele retries; (b) firmware-attempts moeten niet samenvallen met app-attempts 0-3, anders identieke hash (L9); (c) `SEND_CONFIRMED` bij `ACK_S` moet de `ACK_R`-waarde van de laatste app-poging dragen, niet `ACK_S`; (d) met de standaard app ziet Bob "failed" bij een bericht dat al op Alice' radio staat. Gebruikers sturen dan handmatig opnieuw met een nieuwe timestamp, wat Alice' dedup niet vangt.
- **Voorstel**: (a)-(c) specificeren in par. 3/4. Bij (d) de keuze expliciet maken: `SEND_CONFIRMED` al bij `ACK_R` naar de app (gedrag als nu) en "afgeleverd" alleen via 0x91/statuskanaal, of de duplicaten accepteren. Dit schuurt met beslissing 2 en R1-toets; Andy beslist.

### I9. Inbox als bestand per slot op SPIFFS

- **Claim** (`03` par. 6): "ESP32 SPIFFS is ruim (minimaal `min_spiffs.csv`)"; inbox 256 × 176 B = 45 KB; een bestand per slot.
- **Bewijs**: `min_spiffs.csv` heeft een SPIFFS-partitie van 0x20000 = 128 KB (framework `tools/partitions/min_spiffs.csv`), gebruikt door o.a. `variants/lilygo_tbeam_SX1262/platformio.ini:36`, T-LoRa, Xiao C3. De companion gebruikt SPIFFS op ESP32 (`examples/companion_radio/main.cpp:105-106`). Een SPIFFS-bestand kost minimaal een indexpagina plus een datapagina (256 B elk, ESP-IDF-default), dus 256 slots ≈ 128 KB, naast contacten (160 × 152 B op T-Beam) en prefs.
- **Voorstel**: inbox als één of enkele ringbestanden met vaste recordgrootte, of slotaantal afleiden van `SPIFFS.totalBytes()` bij boot (`DataStore.cpp:123`). Heltec V3 (8 MB) is niet het probleem, 4 MB-borden wel. Op nRF52 is `openWrite` remove-then-write (`DataStore.cpp:34-37`), niet crash-veilig voor statusupdates in hetzelfde bestand.

### I10. "Alice gehoord" als trigger is zwak

- **Claim** (`00` R6, `03` par. 5): extra poging als Alice gehoord wordt; aflevering "zodra beide radio's tegelijk bereikbaar zijn".
- **Bewijs**: zie B1; Alice' radio zendt alleen als zij iets verstuurt.
- **Gevolg**: staat Alice' radio 3 uur per dag aan en retryt Bob elke 24 u, dan is de kans per dag circa 1/8. De toets van R6 haalt het ontwerp dus alleen met geluk.
- **Voorstel**: fork-Alice zendt bij boot (en hooguit elke 6 u) een zero-hop advert met `RDM`-bit, of een kleine "ik ben er"-REQ naar contacten met recente fork-berichten. Meet in stap 4 de tijd tot aflevering met een radio die periodiek aan staat.

## Vereenvoudiging (YAGNI)

### Y1. FETCH-antwoord draagt de payload (belangrijk)

Nu: FETCH → hashlijst → per bericht een losse direct-TXT van M → `ACK_R` van Alice naar Bob (vaak flood) → DONE → DONE-antwoord; plus mailboxstates OFFERED/REPLAYED, 10-minutenregel, "max 5 replays" (die met "daarna alleen op FETCH" tegenstrijdig is, want er wordt nooit zonder FETCH gerepliceerd).

Voorstel: `FETCH(prev_hash, prev_ack, prev_result)` → RESPONSE `tag(4) | resterend(1) | inner payload`. Plaintext `4 + 1 + 164 = 169`, past na de fix uit I1 (≤ 176). Alice verwerkt de inner payload via het normale ontsleutelpad, stuurt geen losse `ACK_R` naar Bob, en bevestigt het vorige bericht in de volgende FETCH (DONE en retry van DONE vervallen, lost I6 deels op). Elke RESPONSE heeft een unieke hash (lost I5 op). Antwoord flood als M geen pad heeft; een PATH-return past niet met een lang pad erbij. Kost: een REQ per bericht in plaats van per 8, ongeveer gelijk aan de nu weggelaten DONE-antwoorden.

### Y2. Kleinere schrappingen

- `ACK_R` 4×4 in de outbox is afleidbaar uit de opgeslagen tekst; 16 B per entry en een bron van inconsistentie (B2).
- `STATUS FORGET`: M ruimt toch op na 30 d.
- Timestamp-replaycheck bij Alice voor `RECEIPT_QUERY`: idempotente leesactie, levert niets op en vraagt persistente `last_timestamp` per contact.
- TTL in MBX_INFO dubbelt met `ttl_dagen` in het registratieantwoord.
- Statuskanaal "rdm-status" pas bouwen na de meting van stap 3 (late `SEND_CONFIRMED`); channel-frames worden bij een volle queue als eerste verdrongen (`MyMesh.cpp:224-241`), dus statusregels kunnen stil wegvallen.

## Klein

| # | punt | bewijs / voorstel |
|---|---|---|
| K1 | 32-bit vervalsing: de rate-limit geldt alleen voor STATUS, niet voor losse ACK-pakketten van derden. Met ~208 open waarden (16 × 5 + 128) en een zender die 100% van de tijd ACK's stuurt: circa 6 valse treffers per jaar, bij 10% duty cycle 0,6. | Acceptabel, maar de tekst in `03` par. 1a is onvolledig. Eventueel `ACK_S` alleen via versleutelde kanalen accepteren (PATH-extra, `RECEIPT_QUERY`, STATUS). |
| K2 | Klok zonder RTC: na reboot zonder app zet `bootstrapRTCfromContacts` de klok op de laatste `lastmod + 1` (`BaseChatMesh.cpp:60-70`). Die stilstaande klok vertraagt T_radio/T_sync en kan REQ's laten weigeren door `timestamp < last_timestamp` (`simple_room_server/MyMesh.cpp:541`). | Laatst gebruikte REQ-timestamp persisteren en `max(nu, laatste + 1)` gebruiken; `tijd_M` uit de registratie gebruiken om bij te stellen; timers relatief opslaan. |
| K3 | M moet in Bobs en Alice' `contacts[]` staan voor `sendRequest` (`BaseChatMesh.h:165-166`) en voor peer matching; zichtbaar in de standaard app en onderhevig aan overschrijven bij een volle lijst. | Expliciet maken, of een verborgen peer-tabel in de fork. |
| K4 | UNDECRYPTABLE detecteren kan niet via `onPeerDataRecv`: een pakket zonder matchende peer eindigt in `src/Mesh.cpp:190`. | Hook in `Mesh` nodig (onder flag), of vervalt met Y1 (Alice ontsleutelt de inner payload zelf). |
| K5 | `owner_prefix` is 6 bytes in de registratie en 4 in DEPOSIT. | Eén lengte kiezen. |
| K6 | Gedrag bij volle outbox (16/8) is niet gespecificeerd. | Terugval op gewone DM plus status `NO_OUTBOX`. |
| K7 | Attempt-byte is 8 bits: met "extra poging als Alice gehoord" (min 10 min) zijn 64 varianten per klasse binnen een dag op. | Wrap toestaan; hash-herhaling is na uren onschadelijk, maar noem het. |
| K8 | Inbox-DoS: elk contact kan Alice' 24 slots (nRF52) vullen terwijl haar telefoon weg is; daarna weigert ze alle DM's, ook van standaard afzenders die nu nog "delivered" zien. | Quotum per afzender in de inbox. |
| K9 | Nieuwe push 0x91 gaat ook naar de standaard app; gedrag bij onbekende push codes is niet geverifieerd. | Pas pushen na opt-in via een `CMD_RDM_*`. |
| K10 | Losse `ACK_S` zonder extended/random bytes heeft bij herhaling dezelfde hash en wordt in de ring gedropt. | Zelfde 6-byte vorm als `ACK_R` (`BaseChatMesh.cpp:247-248`). |
| K11 | Een app die berichten op het scherm van het device toont (companion UI) telt niet als sync; zonder telefoon loopt de inbox vol. | Bewust accepteren of UI-weergave als sync laten tellen. |

## Wat klopt

Getoetst en correct: ACK-formule en `attempt & 3` (`BaseChatMesh.cpp:245`, `:457-462`); packet hash zonder path (`src/Packet.cpp:41-50`); onbekende txt_type stil genegeerd zonder ACK (`BaseChatMesh.cpp:308-310`); onbekende req_type zonder antwoord (`MyMesh.cpp:684-687`, `MyMesh.h:102-103`); FEAT1 correct overgeslagen door oude parsers (`AdvertDataHelpers.cpp:51-56`), nu ongebruikt (`AdvertDataHelpers.h:15`); push codes tot 0x90 bezet (`MyMesh.cpp:117-133`); direct ontvangen DM geeft los ACK, flood geeft PATH (`BaseChatMesh.cpp:249-256`); `CMD_SYNC_NEXT_MESSAGE` zonder bevestiging (`MyMesh.cpp:1415-1423`); MBX_INFO 48/52 bytes; 10% duty cycle in 869,4-869,65 MHz; deterministische ciphertext (AES-ECB zonder IV, `01` par. 3), dus een herhaalde DEPOSIT na reboot geeft dezelfde hash en ALREADY_STORED. Niet lokaal geverifieerd: nRF52 InternalFS 28 KB (framework niet aanwezig) en FEAT1-parsing in de standaard app (HIL stap 5).

## Reactie ontwerp (29 sep 2026)

Elke bevinding is tegen de code nagelopen voordat ze is verwerkt in `03-ontwerp.md`, `adr-001-reliable-dm.md` en `00-requirements.md`. Beslissingen van 29 sep zijn intact gebleven.

| # | status | verificatie en verwerking |
|---|---|---|
| B1 | verwerkt | Bevestigd: advert alleen op commando of UI (`MyMesh.cpp:1297`, `NodePrefs.h:195-196`). Capability in-band: trailer `0x00 | attempt | 0x81` in fork-DM's, byte `0x81` achter de ACK van een fork-ontvanger (`03` par. 1b). **Afwijking**: FEAT1 is geschrapt in plaats van als hint behouden; niets in het ontwerp gebruikt het nog (YAGNI), en het scheelt een mogelijke botsing met upstream. PATH-extra van standaard nodes is met nullen gepadded (`src/Mesh.cpp:170-172`), dus een niet-nul cap-byte is onderscheidbaar. |
| B2 | verwerkt | Bevestigd: `temp[4] = attempt & 3` gaat de ACK-hash in (`BaseChatMesh.cpp:457-462`). `ACK_S` nu over `(txt_type << 2)` met attemptbits 0; querysleutel `K = SHA256(timestamp | tekst | pubkey_Bob)[0:4]` (`03` par. 1a, 3). |
| I1 | verwerkt | Bevestigd: `data_len + CIPHER_MAC_SIZE + CIPHER_BLOCK_SIZE-1 > MAX_PACKET_PAYLOAD` (`src/Mesh.cpp:490`) geeft plaintext ≤ 167. Gekozen: exacte check onder flag; grenzen 157 (fork-DM met trailer) en 152 (mailboxkopie), 136 zonder de fix (`03` par. 3). |
| I2 | verwerkt | Bevestigd: `preambleLengthForSF` geeft 32 bij SF ≤ 8 (`RadioLibWrappers.h:56`). Alle tabellen herrekend per pakket inclusief pad-byte; mailboxflow na Y1 7,9 s per hop, zonder mailbox 4,7 s, leeg ophalen 0,8 s/u. De vorige 6,9 s lag 23% onder de gecorrigeerde oude flow en had de 20%-norm niet gehaald (`03` par. 5). |
| I3 | verwerkt | Bevestigd: één token per owner in het schema. `token_B = HMAC(K_owner, pubkey_Bob)[0:8]`, controle en denylist op de Pi. **Afwijking**: "niet auto-added" is niet vast te stellen, `ContactInfo` heeft geen herkomstveld (`src/helpers/ContactInfo.h:8-33`). Wel de favoriet-vlag (`isFav`, `:29`), die de gebruiker bewust zet; MBX_INFO gaat alleen naar favorieten. |
| I4 | verwerkt | Terecht. In CUSTODY een probe naar Alice (elke 24 u en bij horen, min 1 u); UNKNOWN leidt tot een directe DM. STATUS UNKNOWN van M gaat terug naar RETRY (`03` par. 4). |
| I5 | verwerkt | Bevestigd (`src/Mesh.cpp:101`). Opgelost via Y1: elke levering is een RESPONSE met eigen tag en dus eigen hash. |
| I6 | verwerkt | Terecht. Rapporten (ook SYNCED) blijven in elke FETCH staan tot `rapporten_ok` ze bevestigt; STATUS alleen voor berichten die M bewaart, anders `RECEIPT_QUERY`. |
| I7 | verwerkt (aangepast) | Probleem klopt. **Afwijking van het voorstel**: "EVICTED, Bob stopt zonder herzending" kan een bericht verliezen. Voorbeeld: bericht t=100 van Bob is nooit aangekomen, t=150 wel en is later verwijderd; watermerk 150 maakt de query voor t=100 EVICTED. Daarom: EVICTED is alleen eindstatus als Bob al een `ACK_R` had (ON_RADIO), en dan gelijk aan DELIVERED, omdat ongesyncte entries nooit verwijderd worden en het antwoord van Alice zelf komt (versleuteld met ECDH(Alice, Bob)). Zonder eerdere `ACK_R` stuurt Bob het bericht gewoon (`03` par. 1e). |
| I8 | verwerkt (a-c) / besloten (d), beslissing 6 | Bevestigd: app kiest attempt en timestamp (`MyMesh.cpp:1135-1140`), `SEND_CONFIRMED` bevat de ACK uit de tabel (`:417-434`). (a) één entry per `(contact, timestamp, K)`; (b) firmware-attempts vanaf 252 + c aflopend; (c) `SEND_CONFIRMED` draagt `app_ack`. (d) besloten door Andy (29 sep): `SEND_CONFIRMED` bij `ACK_R` (`03` par. 9, beslissing 6). |
| I9 | verwerkt | Bevestigd: `min_spiffs.csv` in het lokale framework heeft `spiffs 0x20000` (128 KB); companion gebruikt SPIFFS op ESP32 (`examples/companion_radio/main.cpp:105-106`); `openWrite` op nRF52 verwijdert eerst (`DataStore.cpp:34-37`). Nu: één recordbestand per structuur, CRC, A/B voor statusrecords, in-place schrijven, aantallen afgeleid uit de FS-grootte (`03` par. 6). |
| I10 | verwerkt (ander middel) | Probleem klopt. **Afwijking**: een zero-hop advert bereikt alleen directe buren, dus niet een Bob die hops verderop zit; een periodieke flood-advert kost bij elke repeater zendtijd. In plaats daarvan stuurt Bob een gebundelde `RECEIPT_QUERY` als probe (0,41 s tegen 1,72 s voor een DM), direct elke 30 min het eerste etmaal en daarna elke 2 u, flood hooguit elke 4 u. R6-toets aangescherpt en te meten in stap 4. |
| Y1 | verwerkt | Klopt, met exacte lengtecheck: FETCH-antwoord `4+1+1+1+164 = 171` bytes plaintext past in 176. Geschrapt: DONE, OFFERED/REPLAYED, 10-minutenregel, "max 5 replays", losse TXT-replay en de flood-`ACK_R` na een mailboxkopie. UNDECRYPTABLE ziet Alice nu zelf (lost K4 op). Eerste FETCH via flood krijgt een PATH-return zonder kopie; daarna direct. |
| Y2 | verwerkt | Alle vijf overgenomen: `ACK_R`'s niet persistent (bij laden berekend), `STATUS FORGET` weg, geen replaycheck op `RECEIPT_QUERY`, TTL uit MBX_INFO, statuskanaal pas na stap 3. |
| K1 | verwerkt (anders) | Berekening klopt. In plaats van `ACK_S` alleen via versleutelde kanalen: `ACK_S` is 48 bits (6 hashbytes in het ACK-pakket), wat de kans met een factor 65.536 verlaagt zonder transport te beperken. |
| K2 | verwerkt | Bevestigd (`BaseChatMesh.cpp:60-70`). Laatst gebruikte REQ-timestamp persistent, `max(nu, laatste + 1)`. `tijd_M` wordt niet gebruikt om de klok te zetten: dan zou M de klok kunnen sturen. |
| K3 | verwerkt | Verborgen peer-tabel (max 4) in de fork. |
| K4 | verwerkt | Vervalt met Y1. |
| K5 | verwerkt | Overal 4 bytes. |
| K6 | verwerkt | Status NO_OUTBOX, gewone DM. |
| K7 | verwerkt | Wrap toegestaan, genoemd in `03` par. 1d; met probes zijn hele herhalingen zeldzamer. |
| K8 | verwerkt | Inboxquotum per afzender: een kwart van de inbox. |
| K9 | verwerkt | 0x91 alleen na `CMD_RDM_ENABLE`. |
| K10 | weerlegd | `ACK_S` wordt niet kort na elkaar herhaald: na de eerste zending alleen via versleutelde antwoorden (`RECEIPT_QUERY`, FETCH-rapport), of opnieuw bij een duplicaat-DM, en die komt pas na een probe of backoff van minimaal 30 min. Een 160-ring op een druk kanaal is dan meestal al doorgedraaid, en anders vangt de volgende query het op. Random bytes zouden de 48-bit sterkte uit K1 kosten. |
| K11 | verwerkt | Weergave op het companion-scherm telt niet als sync, conform beslissing 2; het inboxquotum (K8) begrenst het vollopen. |
