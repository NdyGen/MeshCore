# ADR-001: Betrouwbare DM's via afzender-outbox en ontvanger-gekozen mailbox

- Status: geaccepteerd (29 sep 2026), bijgewerkt na review `04-review.md`
- Beslisser: Andy van Dongen
- Details: `03-ontwerp.md`; requirements `00-requirements.md`; sequenties `05-sequenties.md`; implementatie `06-implementatieplan-v1.md`

## Context

Een MeshCore-DM wordt nergens persistent bewaard: niet bij de afzender (retries zijn app-logica, `examples/companion_radio/MyMesh.cpp:898`), niet onderweg, en bij de ontvanger alleen in een RAM-queue die al geACKt is voordat opslag slaagt (#3518). Is de radio van de ontvanger uit, dan is het bericht na enkele seconden "failed" (`01-huidige-werking.md` par. 7). De bestaande ACK betekent "radio heeft het in RAM", niet "de ontvanger heeft het".

Eisen (R1-R8): E2E ten opzichte van elk tussenstation, statussen "in bewaring" en "afgeleverd" waarvan de laatste niet door een tussenstation te vervalsen is, werken zonder mailbox, onbeperkte opslag via een host, RF-only, en compatibel met standaard nodes zonder versiebits of nieuwe payload types (`01` par. 9).

Upstream wijst store-and-forward op tussennodes af ("room servers are the recommended way", #613). Geen bestaande fork of vergelijkbaar systeem (LXMF, Meshtastic S&F) levert E2E-opslag met een onvervalsbaar afleverbewijs (`02-prior-art.md`).

## Opties

- **A. Alleen afzender-outbox**: persistente retries, dedup en ACK-na-opslag bij de ontvanger. Klein en goed upstreambaar, maar aflevering vraagt dat beide radio's ooit tegelijk bereikbaar zijn en er is geen "in bewaring".
- **B. A plus een door de ontvanger gekozen dedicated mailbox** (radio + Pi) die de originele versleutelde DM ongewijzigd bewaart en direct aan de ontvanger aflevert op haar verzoek.
- **C. Opportunistische S&F op repeaters** (edotassi d8471f3): RAM-ring, replay bij advert. Geen receipt, geen duurzame opslag, upstream afgewezen.
- **D. Room server die ciphertext bewaart**: functioneel gelijk aan B, maar vermengt twee rollen en deelt de ACL van 32 met roomleden.

## Beslissing

We kiezen **B, gebouwd in twee fasen: eerst A, daarna de mailbox.**

Kernkeuzes:
1. De mailbox bewaart de TXT_MSG-payload bit-voor-bit; ze heeft geen sleutel tot de inhoud.
2. Correlatie tussen afzender en mailbox via de packet hash (`src/Packet.cpp:41-50`), tussen afzender en ontvanger via de attempt-onafhankelijke sleutel `SHA256(timestamp | tekst | pubkey_afzender)[0:4]`. De afzender geeft zijn verwachte ACK's nooit aan de mailbox; de mailbox geeft de ACK's van de ontvanger ongewijzigd door en de afzender controleert ze zelf.
3. Twee end-to-end bewijzen van de ontvanger: de bestaande ACK (`ACK_R`, "op Alice' radio", na persistente opslag) en een nieuwe sync-ACK (`ACK_S`, 48 bits, attempt-onafhankelijk, "afgeleverd", zodra haar telefoon het bericht heeft opgehaald). Beide vragen de plaintext; `ACK_S` is niet af te leiden uit `ACK_R`.
4. De mailbox levert af in het antwoord op het ophaalverzoek van de ontvanger (één kopie per antwoord, unieke hash per levering); de ontvanger bevestigt in haar volgende ophaalverzoek. Geen losse replays, geen flood-ACK naar de afzender, geen pad-vervuiling.
5. Capability in-band: een trailerbyte in elke fork-DM en een extra byte achter de ACK van een fork-ontvanger. Geen advert-bits.
6. Voor ontvangers met bekende capability probeert de afzender met een kleine versleutelde statusvraag (`RECEIPT_QUERY`) in plaats van het hele bericht te herhalen.
7. Extensie alleen via bestaande types: nieuwe txt_type (MBX_INFO), nieuwe req_types (DEPOSIT, FETCH, STATUS, RECEIPT_QUERY), ANON_REQ-registratie en losse ACK-pakketten. De lengtecheck in `Mesh::createDatagram` wordt onder flag exact gemaakt.
8. "In bewaring" pas na een bevestigde commit op de Pi.
9. Deposit-token per afzender: `HMAC(K_owner, pubkey_afzender)`, alleen naar favoriete contacten.
10. Alles achter `WITH_RELIABLE_DM`, `RDM_STATUS_CHANNEL` en `WITH_DM_MAILBOX`.

## Beslissingen (29 sep 2026)

| # | onderwerp | besluit |
|---|---|---|
| 1 | Zichtbaarheid "in bewaring" | `PUSH_CODE_RDM_STATUS` (0x91) en `CMD_RDM_*`; optioneel lokaal statuskanaal "rdm-status" achter `RDM_STATUS_CHANNEL` voor de standaard app. |
| 2 | Betekenis "afgeleverd" | Alice' telefoon heeft het bericht opgehaald. Tussenstatus "op Alice' radio" via de bestaande ACK; eindstatus via de nieuwe `ACK_S`. Wijkt af van het oorspronkelijke advies (radio persistent). |
| 3 | Plaats mailbox | Dedicated: eigen Heltec en eigen Pi met `mbxd`, rs01d als codebasis. Niet naast RS01, niet in de room-firmware. |
| 4 | Autorisatie | Token van Alice via MBX_INFO plus quotum per afzender. Na review uitgewerkt als token per afzender (HMAC) en alleen naar favorieten. |
| 5 | Upstream | Eerst bouwen met voorlopige protocolnummers; GitHub-discussie pas met meetdata. #3518-fix als losse PR-kandidaat, nog niet indienen. |
| 6 | Vinkje in de standaard app | `SEND_CONFIRMED` bij `ACK_R`: het vinkje betekent "op Alice' radio". "Afgeleverd" (telefoon) via 0x91 of het statuskanaal. Verworpen: vinkje pas bij `ACK_S` ("failed" tot sync, dubbele handmatige herzendingen). |

## Aanvullingen na de sequenties (29 sep 2026)

De gaten G1-G15 uit `05-sequenties.md` zijn gesloten met concrete regels in `03-ontwerp.md` par. 11. De belangrijkste:

- De afzender kan vanuit elke niet-eindstate direct naar ON_RADIO of DELIVERED op een geldig bewijs, via welk kanaal het ook binnenkomt (probe-antwoord, STATUS, losse ACK).
- Een eigen monotone RDM-klok voor alle deadlines; na een boot volgt een inhaalronde.
- RESYNC wordt afgeleid uit een `store_id` in elke FETCH in plaats van een vlag die de ontvanger zelf moet detecteren.
- Mailboxwissels en mislukte deposits hebben een eigen herhaalschema; capability wordt ingetrokken bij bewijs van standaard firmware, en in RETRY gaat minstens elke 24 u een hele DM.

Voorlopige defaults, toegepast zonder te blokkeren (Andy kan ze herzien):

| # | gat | default |
|---|---|---|
| D1 | G6 | Zonder betrouwbare klok tellen T_radio en T_sync alleen aan-tijd. |
| D2 | G7, G12 | Bij verlies van een registerrecord of van een onbevestigd SYNCED-rapport: liever een mogelijk duplicaat dan verlies. |
| D3 | G2 | Een eindstatus die geen 0x91-client ophaalt, vervalt na 24 u uit de outbox. |

## Gevolgen

Positief
- Aflevering zonder gelijktijdige bereikbaarheid van afzender en ontvanger.
- "Afgeleverd" betekent echt dat de ontvanger het op haar telefoon heeft; de afzender kan "onderweg", "op haar radio" en "gelezen door haar toestel" onderscheiden.
- Het radiobewijs bestaat al en werkt ook met standaard nodes; alleen `ACK_S` is nieuw en wordt alleen naar fork-afzenders gestuurd.
- Fase A lost los al L2, L3, L5, L6, L7 en L13 uit `01` op en is apart upstreambaar, te beginnen met de #3518-fix.
- Ontvanger-gekozen mailbox: geen mailbox-naar-mailbox synchronisatie, dus geen internetkoppeling en geen AUP-risico. Dedicated hardware houdt RS01 en de mailbox onafhankelijk.

Negatief
- Drie firmwarerollen en een extra Pi met Heltec om te onderhouden; de mailbox is een single point of failure voor "in bewaring" (niet voor aflevering: A blijft werken).
- "Afgeleverd" hangt af van gedrag van Alice: opent ze haar app niet, dan blijft Bob op "op Alice' radio" staan tot T_sync (default 30 dagen).
- Alice' radio moet berichten bewaren tot haar telefoon synct: inbox op ESP32 met 8 MB flash 256 berichten, op 4 MB-borden (SPIFFS 128 KB) circa 100, op nRF52 zonder externe flash 24. Een volle inbox weigert nieuwe DM's; quotum per afzender voorkomt dat één contact hem vult.
- In de standaard app zonder statuskanaal is het verschil tussen "op Alice' radio" en "afgeleverd" niet zichtbaar: het vinkje betekent "op Alice' radio" (beslissing 6).
- Sync-detectie leunt op het sync-gedrag van de app (volgend `CMD_SYNC_NEXT_MESSAGE`); bij een afgebroken verbinding kan Alice een bericht twee keer zien.
- Maximaal 157 bytes tekst voor een fork-DM (trailer) en 152 voor een mailboxkopie (normaal 160); de 152 vraagt de exacte lengtecheck in `src/Mesh.cpp` (zonder: 136).
- De mailbox ziet volledige pubkeys, grootte, tijdstippen en ook wanneer Alice synct. Meer metadata dan een repeater.
- Extra zendtijd (preamble 32 bij SF8): 7,9 s per hop per mailboxbericht, 0,8 s per hop per uur voor ophalen, 0,41 s per probe.
- Voorlopige protocolnummers (txt_type 8, req_types 0x41, 0x42, 0x44, 0x45, push 0x91, cap-byte 0x81) kunnen botsen met toekomstig upstreamgebruik; afstemming nodig voor een PR.
- Zonder betrouwbare klok (geen app-verbinding sinds boot, geen RTC-chip) lopen deadlines alleen tijdens aan-tijd (D1): een bericht kan langer in de outbox staan dan 7 dagen kalendertijd.
- De cap-byte is niet geauthenticeerd: een vervalste cap laat de afzender hooguit tevergeefs op `ACK_S` wachten.

## Kantelpunt

Blijken de radio's van afzender en ontvanger na fase A in de praktijk vrijwel altijd tegelijk bereikbaar (vaste thuisnodes), dan stoppen we na A: de mailbox voegt dan alleen "in bewaring" toe tegen de hoogste bouw- en beheerkosten.
