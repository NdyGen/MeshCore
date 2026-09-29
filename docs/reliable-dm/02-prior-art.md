# 02 Prior art: store-and-forward met E2E en receipts

Onderzocht op 29 sep 2026. Claims zijn getoetst aan code of spec; waar dat niet lukte staat "(niet geverifieerd)".
Afkortingen: PN = propagation node, S&F = store-and-forward, E2E = end-to-end.

## Kernbevindingen

1. **Niemand doet E2E-mailbox + twee receipts op LoRa.** LXMF heeft E2E-opslag maar geen "afgeleverd"-receipt via de mailbox; Meshtastic S&F is niet E2E en slaat PKI-DM's helemaal niet op; MeshCore upstream heeft alleen de room server (leest inhoud) en de companion offline queue (RAM).
2. **Iedere volwassen bron scheidt "in bewaring" van "afgeleverd".** DTN BPv6 (custody vs delivery report), LXMF (SENT vs DELIVERED), Briar (`sent` vs `seen`). De mailbox mag de tweede status nooit zelf aanmaken.
3. **MeshCore's bestaande DM-ACK is al een impliciet E2E-bewijs.** De ACK is `sha256(timestamp|attempt|tekst, sender_pubkey)[0:4]`; alleen wie de plaintext heeft kan hem maken. Een mailbox die het versleutelde pakket ongewijzigd bewaart en later aflevert, laat Alice precies dezelfde ACK maken. Het ontbrekende stuk is dat de afzender die ACK na uren nog moet herkennen.
4. **Pull + "recipient seen"-trigger + expliciete delete is het gangbare ophaalpatroon** (LXMF `/get` met wants/haves, Briar list/download/delete, room server `sync_since`). Meshtastic mist de push-trigger en krijgt daar klachten over.
5. **Wat niet past op 184 bytes:** per-bericht Ed25519-handtekening + ephemeral key (LXMF ~210 B overhead), PoW-stamps, link/Resource-transfers, onion-lagen (NNCP). Wel passend: korte message-ID's, gebatchte idempotente ACK's, per-contact quota, TTL.

## 1. Reticulum / LXMF

Bron: LXMF v1.2.0 commit `e52016c`, Reticulum `d5962d1`.
LX = https://github.com/markqvist/LXMF/blob/e52016c20fbbd0bb9082a6e6d5177f5f4eeff1c7/LXMF/
RN = https://github.com/markqvist/Reticulum/blob/d5962d14eb4fbf4a34a0b83e6942534a4965dd17/RNS/

**Formaat en E2E**
- Bericht: `dest_hash(16) | src_hash(16) | Ed25519-sig(64) | msgpack[ts, title, content, fields]`, vaste overhead 112 B (LX `LXMessage.py#L55-L63`, `#L360-L395`).
- Voor propagation wordt alles na `dest_hash` versleuteld naar de ontvanger: `dest_hash | Identity.encrypt(src|sig|payload)` (LX `LXMessage.py#L430-L441`). Encrypt = ephemeral X25519 + HKDF + AES-CBC + HMAC-SHA256, 48 B token-overhead plus 32 B ephemeral pubkey (RN `Identity.py#L807-L836`, `Token.py#L50`). Optionele ratchets geven forward secrecy.
- PN ziet alleen: destination hash, `transient_id = full_hash(dest|blob)`, grootte, ontvangsttijd en de stamp. Afzender blijft verborgen.
- Opgeteld ~210 B overhead vóór content bij propagation (eigen optelling uit bovenstaande velden).

**Afleveringsmethodes** (LX `LXMessage.py#L30-L34`): OPPORTUNISTIC (1 pakket, max 295 B content), DIRECT (Reticulum link, groot via Resource), PROPAGATED (via link naar PN), PAPER (QR/URI). Retry: `MAX_DELIVERY_ATTEMPTS=5`, 10 s wacht (LX `LXMRouter.py#L27-L33`). In de library zelf is er **geen automatische fallback van direct naar propagated**; dat is app-gedrag (Sideband/NomadNet niet geverifieerd).

**Opslag op PN**
- Eén bestand per bericht in `messagestore/`, naam `<transient_id>_<ts>[_<stampvalue>]`; index in RAM, bij boot herbouwd uit bestandsnamen (LX `LXMRouter.py#L557-L590`, `#L2582-L2591`).
- Expiry 30 dagen (`#L40`, `#L1148-L1175`). Opslaglimiet met eviction op gewicht `prioriteit × max(1, leeftijd_dagen/4) × grootte` (`#L1060-L1070`).
- Dedup op `transient_id` (`#L2552-L2570`). Geen per-destination quota gevonden.

**Ophalen (client naar PN)**, `request_messages_from_propagation_node` (LX `LXMRouter.py#L506-L555`):
1. Link naar PN, `link.identify()` bewijst de identiteit van de ophaler.
2. Request `/get` met `[None, None]`: PN antwoordt met transient_id's voor die identiteit, oplopend op grootte.
3. Client stuurt `[wants, haves, transfer_limit]`; PN levert `wants` en verwijdert `haves` (alleen als de destination klopt) (`#L1486-L1580`, delete op `#L1512-L1525`).
4. Na ingest nogmaals `[None, haves]` om te laten wissen (`#L1640-L1653`).

**PN-naar-PN sync** (LX `LXMPeer.py#L267-L395`): peers via announces (max 20), peering vereist PoW-key (`PEERING_COST=18`); `/offer` met lijst transient_id's, antwoord "heb alles / wil alles / lijst"; daarna één Resource. Backoff 12 min per mislukte sync, peer weg na 14 dagen onbereikbaar.

**States en receipts**
- States GENERATING, OUTBOUND, SENDING, SENT, DELIVERED, REJECTED, CANCELLED, FAILED (LX `LXMessage.py#L15-L23`).
- Bij PROPAGATED betekent SENT: PN heeft een Reticulum-proof teruggestuurd (`__mark_propagated`, `LXMessage.py#L580-L588`; PN roept `packet.prove()` aan na stamp-validatie).
- DELIVERED komt alleen van een proof van de eindontvanger (`__mark_delivered`, `#L568-L578`; `delivery_packet` doet `packet.prove()`, `LXMRouter.py#L1991-L1993`).
- **Via propagation wordt DELIVERED nooit bereikt**: bij SENT gaat het bericht uit de outbound queue (`LXMRouter.py#L2778-L2780`, zelf geverifieerd) en de ontvanger stuurt na ophalen niets naar de afzender.
- Proof = Ed25519-handtekening over de packet hash (64 B implicit, 96 B explicit) (RN `Packet.py#L442-L530`, `Identity.py#L946-L957`).

**Stamps en tickets** (LX `LXStamper.py`): PoW over een HKDF-workblock (3000 rondes voor ontvanger-stamp, 1000 voor PN-stamp), kosten geadverteerd in announce; PN weigert te lage stamps. Tickets (16 B, 21 dagen geldig, meegestuurd in eerder bericht) vervangen PoW tussen bekenden. Doel is spam/resource-bescherming, niet expliciet airtime.

**Airtime**: geen LoRa-specifieke keuzes; limieten (256 KB per bericht, 10 MB per sync) zijn op IP/snelle links gericht. Link-opbouw kost meerdere pakketten, timeout 6 s per hop.

## 2. Meshtastic Store & Forward

Bron: firmware master `6d41e279`, protobufs master.
FW = https://github.com/meshtastic/firmware/blob/6d41e279f1f51bd59f687b9d441c1bf47b1594fc/src
Docs: https://meshtastic.org/docs/configuration/module/store-and-forward-module/

**Werking**
- Server alleen op ESP32 (of Linux native) met PSRAM, minimaal 1 MiB vrij (FW `/modules/StoreForwardModule.cpp#L598-L640`).
- Slaat elk gehoord `TEXT_MESSAGE_APP`-pakket op, broadcast en DM, **als plaintext** in een PSRAM-ring (~3/4 van vrij PSRAM) (`StoreForwardModule.cpp#L180-L215`, `#L77-L82`). Niet persistent; ring-overflow reset alle client-cursors.
- Ophalen alleen op verzoek: DM "SF" of protobuf `CLIENT_HISTORY` met window (standaard 240 min, max 25 berichten). Replay 1 pakket per 5 s en alleen bij <25% channel utilisation, met `want_ack=false` (`#L36-L44`, `#L276-L295`, `#L476-L496`). Per client een `last_request`-cursor.
- Niet toegestaan op het default channel (`#L484`). Optionele heartbeat elke 900 s.

**E2E en DM's**
- PKI-DM's (X25519 naar ontvanger) worden alleen ontsleuteld als ze aan de node zelf gericht zijn (FW `/mesh/Router.cpp` ~L957); de S&F-server kan ze niet lezen en **slaat ze dus niet op**.
- Feature request "Store and Forward DMs" gesloten zonder implementatie: https://github.com/meshtastic/firmware/issues/7610
- Wat wel wordt opgeslagen (channel-PSK-tekst) kan de server lezen. Geen E2E tegen de operator.

**Receipts**
- Afzender krijgt nooit "opgeslagen"; replays hebben geen ACK, dus de server weet ook niet wat aankwam.
- Unicast `want_ack`: ontvanger stuurt Routing-ACK. Maar een gehoorde rebroadcast door een relay telt als impliciete ACK (FW `/mesh/ReliableRouter.cpp#L58-L80`), dus "delivered" kan van een relay komen.
- Nieuw in protobufs master: `Routing.ack_proof = HMAC-SHA256(shared_key, "ack"|from|to|request_id|routing)` zodat alleen de ontvanger een receipt kan maken (https://github.com/meshtastic/protobufs/blob/master/meshtastic/mesh.proto#L1224-L1241, zelf geverifieerd). De firmware-code noemt als bekend gat dat de impliciete relay-ACK de pending entry al wist voordat de echte proof arriveert.

**Waarom beperkt** (docs + issues): alleen PSRAM-ESP32; RAM-only (flash/SD-opslag nooit gemerged: https://github.com/meshtastic/firmware/issues/3234, https://github.com/meshtastic/firmware/pull/5083); alleen pull, geen push bij terugkeer van de ontvanger; duplicaten bij replay (https://github.com/meshtastic/firmware/issues/6332, open); verkeerde timestamps (https://github.com/meshtastic/firmware/issues/4166); geen outbox die doorstuurt (https://github.com/meshtastic/firmware/issues/11553).

**Nieuwere pogingen, niet gemerged**: Store-and-Forward++ (SQLite, hash-chain over primary channel, alleen Linux) https://github.com/meshtastic/firmware/pull/9041 (gesloten 2026-09-19); packet replay door repeaters https://github.com/meshtastic/firmware/pull/8049 (gesloten 2026-08-22). Beide alleen channel-verkeer.

## 3. MeshCore zelf

MC = https://github.com/meshcore-dev/MeshCore

**Issues en PR's**
- #613 (gesloten 2025-08-31) "repeaters caching msgs": liamcottle: "room servers are the recommended way ... not planned to store direct messages on intermediate nodes, as the sender would see it as failed due to the packet timeouts." 446564: een repeater weet niet of hij de laatste hop is. MC/issues/613
- #3077 (open) + PR #3078 (open, niet gereviewd): repeater bewaart 32 GRP_TXT-pakketten versleuteld in RAM, ophalen via ANON_REQ sub-type 0x04 met `anon_limiter`. Alleen channels, dus buiten het ACK-probleem van #613. MC/issues/3077, MC/pull/3078
- #3518 (open, door Andy): companion ACKt een DM en dropt hem als de offline queue vol is. Precedent #282 (gesloten 2025-08-06): liamcottle wilde toen "some other ack/response" voor "inbox vol", recrof "ack with extra flag"; opgelost door queue van 16 naar 256. MC/issues/3518, MC/issues/282
- #3225 (open) + PR #3447: door het device zelf verstuurde berichten naar de app syncen via de offline queue. Geen S&F. MC/issues/3225
- DM-betrouwbaarheid (geen mailbox): #1489 (ACK via inkomend pad + flood), #1834 ("keep sending the message until you get an ack"), PR's #3260, #2569, #2980, #2501, #2367, #3359. PR #3362: `CMD_SYNC_NEXT_MESSAGE` haalt uit de queue vóór het schrijven slaagt.
- Discussion #2851 (room servers als vervanging van channels) is het dichtstbijzijnde; geen discussion over mailbox. MC/discussions/2851
- Roadmap: alleen de README "Roadmap / To-Do" (MC/blob/main/README.md); geen S&F, mailbox of offline DM. Geen andere roadmap- of Discord-samenvatting online gevonden.

**Forks en projecten**
- edotassi/MeshCore d8471f3 (https://github.com/edotassi/MeshCore/commit/d8471f3c7f85f2446396339a4bd079a6a88525e9): repeater bewaart `PAYLOAD_TYPE_TXT_MSG`-pakketten ongewijzigd (dus versleuteld) voor een whitelist van companion key-id's, dedup op packet hash, replay als flood bij een advert van die companion. Geen receipt naar de afzender.
- Geen andere firmware-fork met DM-mailbox gevonden (`gh search repos/code`, web). Wel client/observer-opslag: https://github.com/folkertvanheusden/meshcore-store, https://github.com/jkingsman/Remote-Terminal-for-MeshCore, https://github.com/mwolter805/meshcore-ha-chat
- meshcore_py (https://github.com/meshcore-dev/meshcore_py): `get_msg()` loopt `CMD_SYNC_NEXT_MESSAGE` tot `NO_MORE_MSGS` (`src/meshcore/commands/messaging.py:15`), auto-fetch op MSG_WAITING (`src/meshcore/meshcore.py:452-486`). Geen "mailbox"/"inbox". meshcore.js idem (`src/connection/connection.js:1078`, `:1135`). meshcore-cli niet doorzocht (rate limit).

**Bestaande mechanismen in de code** (deze repo)
- Companion offline queue: RAM-array, 16 default, 256 in de meeste envs (`examples/companion_radio/MyMesh.h:61-62`, `:277-278`). Vol: oudste channel-bericht eruit, anders nieuwe frame stil gedropt (`examples/companion_radio/MyMesh.cpp:224-247`). Pull via `CMD_SYNC_NEXT_MESSAGE`, geen peek of ack vanuit de app.
- DM-ACK: afzender verwacht `sha256(ts|attempt|tekst, eigen pubkey)[0:4]` (`src/helpers/BaseChatMesh.cpp:458-462`); ontvanger roept eerst `onMessageRecv()` (void) aan en bouwt dan dezelfde hash (`src/helpers/BaseChatMesh.cpp:239-256`). Bij flood-ontvangst gaat de ACK mee in een PATH-return. Omdat de hash over de plaintext gaat, kan een tussenstation zonder sleutel hem niet maken: het is een impliciet E2E-bewijs van ontsleuteling.
- Afzender onthoudt verwachte ACK's in een circulaire RAM-tabel van 8 (`examples/companion_radio/MyMesh.h:285`, `MyMesh.cpp:1161-1164`); match geeft `PUSH_CODE_SEND_CONFIRMED` (`MyMesh.cpp:417-434`). Een ACK die uren later komt matcht alleen als er intussen minder dan 8 berichten verstuurd zijn en er geen reboot was.
- Room server: per client `sync_since`, `pending_ack` (sha256 over post + client pubkey), push één post per ronde, na 3 mislukte pushes overgeslagen, ring van 32 (`examples/simple_room_server/MyMesh.cpp:75-131`, `:1000-1035`, `MyMesh.h:68-69`). Dit is een werkend push-met-ACK-patroon, maar op plaintext.
- Docs: `docs/faq.md:145-148` (repeater bewaart niet, room server wel).

## 4. DTN Bundle Protocol

- **BPv6, RFC 5050** (https://www.rfc-editor.org/rfc/rfc5050): custody transfer §5.10: een custodian houdt de bundle vast tot een volgende custodian accepteert; custody signals (succeeded/failed + reden, o.a. "depleted storage") §6.1. Status reports §6.1.1: received, custody accepted, forwarded, delivered, deleted. Custody is hop-by-hop, delivery is end-to-end: twee verschillende signalen.
- **BPv7, RFC 9171** (https://www.rfc-editor.org/rfc/rfc9171): custody uit de kern gehaald en verplaatst naar bundle-in-bundle encapsulation (Appendix A). Status reports blijven (received, forwarded, delivered, deleted) maar staan standaard uit: één bundle kan `1 + 2(N-1)` reports opleveren en kwaadwillig aanvragen is een DoS-vector (§8). De BIBE-custody-draft is inmiddels expired/vervangen (https://datatracker.ietf.org/doc/draft-ietf-dtn-bibect/).
- **BPSec, RFC 9172/9173**: payload versleutelbaar (AES-GCM), primary block (bron, bestemming, report-to, lifetime) blijft leesbaar voor routering.
- Les: receipts moeten optioneel, geauthenticeerd en begrensd zijn; routeringsmetadata blijft zichtbaar, de rest niet.

## 5. Briar, NNCP, Signal

**Briar Mailbox** (https://code.briarproject.org/briar/briar-mailbox, API: `API.md` in die repo)
- Altijd-aan node (Android of Java-host) via Tor, gekoppeld aan één eigenaar via QR-setup-token.
- Per contact een `inboxId`/`outboxId` en 32-byte token, door de eigenaar aangemaakt. Contacten uploaden opake bestanden; eigenaar list, downloadt en verwijdert. De mailbox heeft geen sleutels tot de inhoud (bestanden zijn al door de Bramble-laag versleuteld).
- Status in de app: `MessageStatus` kent `sent` en `seen` (https://code.briarproject.org/briar/briar/-/raw/master/bramble-api/src/main/java/org/briarproject/bramble/api/sync/MessageStatus.java). `seen` komt van een BSP-ACK van het toestel van de ontvanger. Of Briar in de UI "in mailbox" apart toont: niet geverifieerd.
- BSP (https://code.briarproject.org/briar/briar-spec/-/blob/master/protocols/BSP.md): records ACK, MESSAGE, OFFER, REQUEST, VERSIONS. ACK's bundelen meerdere message-ID's en zijn idempotent.

**NNCP** (https://nncp.mirrors.quux.org/ACK.html): pakketten versleuteld naar de bestemmingsnode, relaying via geneste "transitional" pakketten. ACK-pakket bevat het packet-ID; bij ontvangst verwijdert de afzender zijn bewaarde uitgaande kopie ("Each ACK packet will remove kept corresponding outbound packets"). ACK is een gewoon geauthenticeerd pakket, per node-relatie.

**Signal** (niet in detail geverifieerd): server bewaart opake ciphertext tot ophalen; delivery/read receipts zijn zelf E2E-versleutelde berichten van de ontvanger, de server maakt geen receipts.

## 6. Vergelijking

| | Opslag | E2E t.o.v. opslag | "In bewaring" | "Afgeleverd" | Ophalen | Push bij terugkeer |
|---|---|---|---|---|---|---|
| LXMF PN | disk, 30 d, eviction | ja (ciphertext + dest hash) | ja (proof van PN) | nee via PN | pull `/get` wants/haves | nee (client pollt) |
| Meshtastic S&F | PSRAM ring | nee; PKI-DM's niet opgeslagen | nee | alleen direct, relay-ACK ambigu | pull CLIENT_HISTORY | nee |
| MeshCore room server | RAM ring 32 | nee | n.v.t. | ACK van client op push | push na login | ja (login) |
| MeshCore companion queue | RAM 256 | n.v.t. (eigen radio) | nee | ACK al vóór opslag (#3518) | pull door app | MSG_WAITING |
| edotassi fork | RAM ring 500 | ja (pakket ongewijzigd) | nee | nee | push bij advert | ja |
| DTN BPv6 | persistent | met BPSec | custody signal | delivery report | n.v.t. | n.v.t. |
| Briar mailbox | disk | ja | niet geverifieerd | `seen` via BSP-ACK | pull list/download/delete | nee |
| NNCP | spool | ja | nee | ACK-pakket | toss | n.v.t. |

## 7. Overdraagbaar naar MeshCore (binnen R1-R8)

**Overnemen**
- Bewaar het **originele DM-pakket ongewijzigd** (edotassi-aanpak, LXMF-principe dat de mailbox alleen dest hash ziet). Dan blijft E2E (R3) en maakt Alice bij aflevering exact de ACK die Bob al verwacht: geen nieuw receipt-formaat nodig voor "afgeleverd" (R5), wel persistente opslag van de verwachte ACK bij Bob (R6).
- **Aparte "in bewaring"-ACK van de mailbox** (LXMF SENT, BPv6 custody). Moet onderscheidbaar zijn van Alice's ACK; hier kan een nieuw payload-subtype of een ACK met vlag komen (idee uit #282: "ack with extra flag"). Alleen tussen fork-nodes (R2).
- **Pull met wants/haves + expliciete delete** (LXMF `/get`, Briar DELETE), plus **push-trigger op advert/pakket van de ontvanger** (edotassi, room server login). Met korte ID's (4 B packet hash) past een lijst in één pakket.
- **Idempotente, gebatchte ACK's** (Briar BSP, NNCP) en dedup op packet hash (LXMF transient_id, edotassi).
- **Airtime-gating** van replays: 1 pakket per N s, alleen onder een channel-utilisation-drempel (Meshtastic S&F), `anon_limiter`-achtige rate limit (PR #3078).
- **TTL en eviction** (LXMF 30 dagen, gewicht leeftijd × grootte); bij host-opslag (R7) is ruimte geen probleem maar TTL blijft nodig.
- **Per-contact autorisatie/quota** in plaats van PoW (Briar tokens, LXMF tickets): mailbox accepteert alleen voor geregistreerde eigenaren.
- **Hergebruik room-server push-logica** (`pending_ack`, `push_failures`, round-robin) als skelet voor mailbox-aflevering, maar op ciphertext.

**Vermijden**
- Receipts die door relays of de mailbox gesynthetiseerd kunnen worden (Meshtastic implicit ACK, BP status reports). "Afgeleverd" alleen op Alice's plaintext-afgeleide ACK.
- Per-bericht handtekening of ephemeral key (LXMF), PoW-stamps, link-handshakes en Resource-transfers: te duur voor 184 B en nRF52/ESP32.
- PN-naar-PN sync over RF: kost veel airtime en raakt de dutchmeshcore.nl-AUP bij internetkoppeling (R4); hooguit later en optioneel.
- Plaintext-opslag (Meshtastic S&F, room server): strijdig met R3.
- ACK vóór opslag (#3518): mailbox mag "in bewaring" pas melden na een geslaagde write.

**Open vragen voor het ontwerp**
- Hoe weet Bob welke mailbox Alice gebruikt (advert-veld, contact-attribuut of handmatig)?
- De ACK-hash bevat het attempt-nummer; een replay van poging 1 geeft de ACK van poging 1. Bob moet dus alle verstuurde attempt-varianten bewaren.
- Flood-ACK van Alice na replay moet Bob bereiken als die offline is: via dezelfde mailbox terug (Bob's mailbox) of Bob's outbox retry (R6).

## 8. Niet geverifieerd

- Sideband/NomadNet: automatische fallback naar propagation en eventuele eigen E2E-receipt via PN.
- Briar UI-status "via mailbox".
- Meshtastic `ack_proof`: in welke release-tag het zit.
- meshcore-cli code search (rate limit).
