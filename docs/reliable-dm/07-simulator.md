# 07: Native multi-node simulator

Doel: integratietests zonder hardware die de echte protocolcode draaien, zodat betrouwbare DM's (`03-ontwerp.md`, `06-implementatieplan-v1.md`) aantoonbaar werken. Basis: branch `feature/reliable-dm`, 29 sep 2026. Geen wijzigingen in `src/` of `examples/`; alleen `test/`, `platformio.ini` en dit document.

## 1. Draaien

```sh
pio test -e native_sim                      # alle simulatorsuites (test/test_sim_*)
pio test -e native_sim -f test_sim_dm       # één suite
SIM_TRACE=1 .pio/build/native_sim/program '--gtest_filter=*TwoHop*'   # na een build: TX/RX-trace per frame
SIM_VERBOSE=1 ...                           # ook Serial-uitvoer van de firmware
```

- De eerste run downloadt `rweather/Crypto`, `densaugeo/base64` en googletest (`platformio.ini`, `[env:native_sim]`).
- `pio test -e native` negeert `test_sim_*` (en `test_rdm_*`), zodat de bestaande unit tests met hun mocks (`test/mocks`) ongewijzigd blijven. `native_sim` gebruikt die mocks niet: nep-SHA256/AES zou het protocol ongeldig maken.
- Resultaat nu: 21 tests in 3 suites groen (`native_sim`), samen ~12 s inclusief incrementele build. RDM-scenario's draaien in `[env:native_rdm]` (zelfde simulator, plus `-D WITH_RELIABLE_DM`), zie par. 3 `RdmSimCompanion`. Eenmalig ook met ASan/UBSan gedraaid: geen fouten in simulator of firmware; UBSan meldt alleen "left shift of negative value" in de vendored `lib/ed25519`.
- CI: `.github/workflows/run-unit-tests.yml` draait alleen `native` en `native_kiss_modem`. Voorstel (niet gedaan, buiten scope): `-e native_sim` toevoegen.

## 2. Architectuur

```
 test (googletest)
   |  sim.add<ChatNode|RepeaterNode|CompanionNode>(), sim.link(), node.power_off(), app.send_text() ...
   v
 Simulator ------------- virtuele tijd (ms), medium, links, drop-filters, tx_log, fast-forward
   | per tick: frames afleveren, dan node.loop() per aan-staande node (vaste volgorde)
   v
 SimNode (per device)
   RAM, per boot opnieuw:  SimRadio  SimMillis  SimRTC  StaticPoolPacketManager  SimpleMeshTables  mesh
   flash, blijft:          SimFS (pad -> bytes)   SimRNG (seed uit sim-seed, node-index, bootnummer)
   |
   +-- ChatNode       echte BaseChatMesh + test-API (sent/inbox, app-achtige retries)
   +-- RepeaterNode   echte mesh::Mesh met allowPacketForward() = true
   +-- CompanionNode  echte examples/companion_radio MyMesh + DataStore op SimFS, plus CompanionApp (nep-telefoon)
```

Echte code in de build (`[env:native_sim]` `build_src_filter`): `src/Dispatcher.cpp`, `Mesh.cpp`, `Packet.cpp`, `Utils.cpp`, `Identity.cpp` (met `lib/ed25519` en rweather `Crypto`: echte Ed25519, X25519-ECDH, AES-128, HMAC-SHA256), `helpers/BaseChatMesh.cpp`, `StaticPoolPacketManager.cpp`, `AdvertDataHelpers.cpp`, `TxtDataHelpers.cpp`, `IdentityStore.cpp`, `TransportKeyStore.cpp`, `ConfigSerializer.cpp`, `DynamicConfigSerializer.cpp`, `CommonRadioPrefs.cpp`, `examples/companion_radio/MyMesh.cpp` en `DataStore.cpp`.

| bestand | rol |
|---|---|
| `test/sim/Sim.h`, `Sim.cpp` | `Simulator`, `SimNode`, `SimRadio`, `SimRTC`, `SimMillis`, `SimRNG`, medium en fast-forward |
| `test/sim/SimFS.h` | flash van een node, capaciteit en torn-write-budget |
| `test/sim/ChatNode.{h,cpp}` | `ChatNode`, `RepeaterNode`, `AppRetry` |
| `test/sim/CompanionNode.{h,cpp}` | `CompanionNode`, `CompanionApp`, `SimSerialLink` (BLE-link als frame-queues) |
| `test/sim/SimArduino.cpp`, `SimFSArduino.cpp`, `SimTarget.cpp` | Arduino-runtime (`millis`, `random`, `Serial`), `fs::FS`/`File`, `board`/`radio_driver`/`sensors`/`LittleFS`/`radio_new_identity` |
| `test/sim/shim/` | headers die de firmware verwacht: `Arduino.h`, `Stream.h`, `FS.h`, `LittleFS.h`, `target.h`, `CayenneLPP.h`, `RTClib.h` |
| `test/test_sim_dm/`, `test_sim_companion/`, `test_sim_infra/` | de suites |

Build-keuzes in `[env:native_sim]`:
- `-D RP2040_PLATFORM`: de enige platformmacro waarmee `IdentityStore.h` en `DataStore.cpp` een generiek Arduino `fs::FS`-pad kiezen zonder vendor-SDK. `ESP32` zou rweather Crypto naar hardware-AES en `esp_random` sturen. Het bestandssysteem zelf is dat van de simulator.
- `-D MAX_CONTACTS=350 -D MAX_GROUP_CHANNELS=40` zoals de Heltec V3-companion. `OFFLINE_QUEUE_SIZE` blijft op de default 16 (`examples/companion_radio/MyMesh.h:61-62`); Heltec V3 BLE bouwt met 256 (`variants/heltec_v3/platformio.ini:170`).
- `lib_compat_mode = off`: rweather Crypto declareert alleen framework `arduino`.

## 3. API

### Simulator

| aanroep | effect |
|---|---|
| `Simulator s(seed, RadioConfig{sf, bw_khz, cr, preamble})` | default SF8 / 62,5 kHz / CR 4/8 / preamble 32 (NL-instellingen, `RadioLibWrappers.h:56`); één instantie tegelijk |
| `s.add<T>("naam")` | maakt en boot een node |
| `s.link(a, b, snr=8, loss=0)`, `link_oneway`, `unlink`, `linked` | gerichte links met SNR en verlieskans |
| `s.run_for(ms)`, `s.run_until(pred, timeout_ms)` | tijd laten lopen; `run_until` checkt elke tick |
| `s.set_collisions(bool)` | overlappende ontvangst bij één ontvanger: allebei weg (default aan) |
| `s.set_fast_forward(max_step_ms, settle_ms=40000)` | eis S3, zie par. 4 |
| `s.drop_next(type, from=-1, to=-1, n=1)`, `add_drop_filter(fn)`, `clear_drop_filters()` | eis S5: gericht één pakket (per ontvanger) laten vallen |
| `s.tx_log()`, `count_tx(from, type)` | elke transmissie: `seq`, `t_ms`, afzender, header, type, flood, `airtime_ms` (eis S4), ruwe bytes |
| `s.fs_log()` | elke flash-schrijfactie (`FsWrite`: `seq`, `t_ms`, node, pad, offset, lengte, `complete`). `seq` is één teller met `TxRecord::seq`, dus "ACK pas na de inbox-write" is `write.seq < tx.seq` |
| `s.dropped_collision/loss/halfduplex/off/filter()` | tellers per verliesoorzaak |
| `s.now()`, `s.wall_epoch()`, `s.airtime_ms(len)` | simtijd, "echte" klok (start 2026-09-29), LoRa-zendtijd |
| `Simulator::OnNode ctx(s, node)` | code buiten de node-loop "op" een node draaien (`millis()`, `random()`) |

### SimNode (alle nodes)

`power_off()` / `power_on()` (RAM weg, flash blijft), `reboot(wipe_fs)` (wipe = fabrieksreset, nieuwe identiteit), `set_rtc_battery(bool)`/`has_rtc_battery()`, `sync_clock()` (wat een app bij verbinden doet), `rtc().setSinceBoot()` (klok sinds boot gezet: wat firmware "betrouwbaar" mag noemen, G6) en `rtc().markBootstrapped()` (een eigen bootstrap van de firmware telt niet als sync), `fs()` (bewerkbare `SimFS`, eis S1), `rtc()`, `radio()`, `identity()`, `uptimeMs()`, `bootCount()`. `board.reboot()`/`powerOff()` vanuit de firmware wordt na afloop van die loop uitgevoerd (`request_reboot`).

Eigen node (eis S2, bv. `RdmSimCompanion`): erf van `SimNode` en implementeer `createMesh()` met `radio()`, `millisClock()`, `rng()`, `rtc()`, `packetManager()`, `tables()`. Let op: `Mesh::begin()` en `BaseChatMesh::loop()` zijn niet virtueel, dus override `beginMesh()`/`loopMesh()` zodra de mesh-klasse een eigen `begin()` of een `BaseChatMesh`-`loop()` heeft (zie `ChatNode::loopMesh`, `CompanionNode::beginMesh`). Voor fast-forward: `busy()` en `nextWakeupMs()`.

### ChatNode en RepeaterNode

- `advert(flood)`, `add_contact(node)` (zonder advert), `knows`, `has_path_to`, `path_len_to`, `reset_path`.
- `send_text(to, text, attempt, ts)` geeft een id; `sent(id)` met `status` (PENDING, DELIVERED, TIMED_OUT, SEND_FAILED), `expected_ack`, `est_timeout_ms`, `flood`, `timed_out_before_ack`.
- `send_text_with_retries(to, text, AppRetry{max_attempts=3, flood_on_last=true})`: gedrag van een app (attempt+1 na elke time-out, path reset voor de laatste poging). De echte app zit niet in deze repo; dit is een expliciet model.
- `inbox()` met afzender-index, tekst, sender timestamp, flood en pad-lengte.
- Timeouts en contactopslag zoals de companion (`MyMesh.cpp:108-111`); contacten staan in `/contacts` op de SimFS.

### CompanionNode en CompanionApp

`CompanionNode` draait `MyMesh` + `DataStore` in dezelfde volgorde als `main.cpp` (`store.begin()`, `begin()`, `startInterface()`). `app()` is een nep-telefoon op het companion protocol:
- `connect()`: `CMD_DEVICE_QUERY` (v3), `CMD_APP_START`, `CMD_SET_DEVICE_TIME` met de simklok, daarna sync. `disconnect()`; na een reboot van de radio is de link weg.
- Eén commando tegelijk in de lucht; `auto_sync` haalt op `PUSH_CODE_MSG_WAITING` berichten op tot `NO_MORE_MESSAGES`.
- `send_text`, `send_text_with_retries`, `sent(id)` (status via `RESP_CODE_SENT` en `PUSH_CODE_SEND_CONFIRMED`, time-out na `est_timeout`), `inbox()`, `advert(flood)`, `reset_path`, `push_log()`/`push_count(code)`.
- Met `WITH_RELIABLE_DM` (`native_rdm`) krijgt `MyMesh` een `SimFileIO` op `fs()` mee (H5). `rtcTrusted()` leest de klokvlag van de firmware (H1), `setHardwareRTC` volgt `has_rtc_battery()` en `nextWakeupMs()` neemt `rdm().nextWakeupMillis()` mee. Die aanroepen worden gedetecteerd, zodat de simulator ook bouwt tegen een `MyMesh` zonder deze uitbreiding.
- `send_command(frame, on_response)`: elk ander commando, bv. `CMD_RDM_ENABLE` en de 0x91-pushes uit `06` par. 3.14 (eis S6). `on_response` krijgt elk antwoordframe; een commando met START..END-antwoord (`CMD_GET_CONTACTS`, `CMD_RDM_LIST_OUTBOX`) blijft in de lucht tot het afsluitende frame of een ERR.

### RdmSimCompanion (`test/rdm_support/`, alleen `native_rdm`)

Fork-companion: `RdmChatMesh` (dezelfde integratielaag als `MyMesh` onder `WITH_RELIABLE_DM`) op een `SimNode`, met de app-kant als directe aanroepen in plaats van frames. De send- en sync-recepten zijn die van de companion: `onAppSend` -> DM met trailer via `sendTxtPlain` -> `onAppTransmitted`; per sync-verzoek eerst `onSyncRequest`, dan `nextInboxFrame`/`onInboxFrameHanded`.

- `connect(rdm_client)` (zet de klok, `onClientConnected`, synct), `disconnect()`, `auto_sync` op MSG_WAITING.
- `send_text(to, text, attempt, ts)` -> `sent(id)` met `app_ack` en `handled`; `statuses()`/`statuses_for(app_ack)` (0x91-pushes, alleen met `rdm_client`), `confirms()` (`SEND_CONFIRMED`), `inbox()`.
- `advert`, `add_contact(node, favourite)`, `knows`, `has_path_to`, `set_mailbox(pub, k_owner)`, `rdm()` (bijv. `setEnabled(false)` voor een standaard Alice).
- `removed_contacts()`: contacten die `RdmChatMesh` verwijderde omdat ze een verborgen peer (mailbox) werden (K3); de contacten worden dan ook opnieuw opgeslagen.
- RDM-bestanden gaan via `SimFileIO` en verschijnen in `fs_log`; `nextWakeupMs()` komt van `Node::nextWakeupMillis` (H4), dus fast-forward landt op de RDM-schema's.

### Voorbeeld

```cpp
Simulator s(42);
auto& alice = s.add<CompanionNode>("alice");
auto& rep = s.add<RepeaterNode>("rep");
auto& bob = s.add<ChatNode>("bob");
s.link(bob, rep); s.link(rep, alice);
alice.app().connect(); alice.app().advert(true); s.run_for(5000);
bob.advert(); s.run_for(5000);
alice.power_off();
int job = bob.send_text_with_retries(alice, "ben je er?");
s.run_until([&] { return bob.retry_done(job); }, 120000);   // TIMED_OUT: niemand bewaart het
```

## 4. Modellen

**Radio.** `startSendRaw` zet het frame op het medium; elke ontvanger met een link krijgt het na de zendtijd (Semtech-formule, expliciete header, CRC aan; 38 bytes = 509 ms bij de defaults). Verlies door: ontvanger uit of opnieuw geboot tijdens het frame, ontvanger zendt zelf (half-duplex), overlap met een ander frame bij dezelfde ontvanger (geen capture effect), `loss`-kans, drop-filter. `isReceiving()` is waar zolang een frame bij die node binnenkomt, dus de CAD-achtige wachtlus in `Dispatcher::checkSend` (`src/Dispatcher.cpp:289-304`) werkt. `packetScore` is dezelfde formule als `RadioLibWrapper::packetScoreInt` (`RadioLibWrappers.cpp:233-242`), dus flood-ontvangers met lage SNR wachten langer (rx-delay) zoals echt.

**Tijd.** Eén globale klok in ms. Per tick: eerst frames afleveren waarvan de zendtijd om is, dan `loop()` van elke aan-staande node in vaste volgorde. `millis()` en `SimMillis` geven per node de uptime (0 bij boot). Duty-cycle-budget en retransmit-vertragingen zijn de echte uit `Dispatcher`/`Mesh`.

**Fast-forward (S3).** Uit tenzij `set_fast_forward(max_step)`. Dan springt de klok in stappen tot `max_step` zolang: geen frame in de lucht, `settle_ms` (default 40 s, dekt de flood rx-delay tot 32 s, `Dispatcher.cpp:11`) sinds de laatste transmissie, geen node `busy()` (sim-pool niet leeg; companion: `MyMesh::hasPendingWork()` of app-commando's), geen frame in een rx-buffer. De stap eindigt op de vroegste `nextWakeupMs()` van alle nodes. `test_sim_infra` bewijst dat een scenario met 1 uur stilte met en zonder fast-forward een byte- en tijd-identieke `tx_log` geeft, en dat 8 dagen 0,3 s kosten.

**RTC.** Eerste boot: wandklok (alsof de app de tijd zette; `setSinceBoot()` is dan nog onwaar, pas `sync_clock()` of een app-connect zet hem). Reboot zonder batterij: `RTC_RESET_EPOCH` = 15 mei 2024, de waarde van `VolatileRTCClock` (`src/helpers/ArduinoHelpers.h:11`). Met batterij loopt de klok door tijdens uit. Let op: de companion zet zijn klok bij boot op de nieuwste `lastmod` van zijn contacten (`MyMesh.cpp:1001`, `BaseChatMesh.cpp:60-70`), dus die staat na een reboot niet in 2024 maar loopt de uitval achter.

**Flash.** `SimFS` is een map pad -> bytes per node, blijft over reboots, weg bij `reboot(true)`. Schrijven is direct duurzaam. `capacity_bytes` (default 1 MB; 128 KB emuleert een 4 MB-ESP32 met `min_spiffs`) en `fail_writes_after(n, mode)`: na n bytes mislukt elke schrijfactie (`FAIL_WRITES`, de firmware ziet een fout), of de node valt direct na die loop uit (`POWER_OFF`, crash midden in een schrijfactie; eenmalig). `fs::File` ondersteunt `r`, `w`, `a`, `r+`, `w+`, `seek`/in-place schrijven, directory-iteratie. De `SimFileIO` van WP0 (`test/rdm_support/SimFileIO.h`) gebruikt dezelfde budget- en capaciteitsvelden.

**Determinisme.** Alle randomness (identiteiten, retransmit-jitter, verlies) komt uit splitmix64 met seed uit (sim-seed, node-index, bootnummer). Zelfde seed geeft een identieke run (`SameSeedGivesIdenticalRun`).

## 5. Tests en wat ze vastleggen

| test | bewijst (huidig upstreamgedrag) |
|---|---|
| `SimDmBaseline.NeighboursFloodThenDirectDmAreAcked` | eerste DM flood, ACK in PATH, Bob leert pad; tweede DM direct en geACKt |
| `SimDmBaseline.TwoHopDmViaRepeater` | B-R-A: flood via repeater (pad 1 hop), daarna direct via R, beide geACKt |
| `SimDmBaseline.HiddenTerminalCollisionLosesAck` | zie par. 6: DM komt aan, ACK botst bij R, Bob ziet time-out (L6) |
| `SimDmBaseline.DmIsLostWhenRecipientRadioIsOff` | L1: 3 app-pogingen, daarna niets; Alice aan na 10 min krijgt niets, niemand zendt opnieuw |
| `SimCompanion.DmBetweenTwoCompanionAppsIsConfirmedAndSynced` | echte companion aan beide kanten: `SEND_CONFIRMED` bij Bob, bericht via sync in Alice' app |
| `SimCompanion.Issue3518_FullOfflineQueueStillAcksAndDropsDm` | #3518 op de echte firmware: 17 DM's allemaal geACKt, app krijgt er 16, de laatste is weg |
| `SimCompanion.RebootBeforeAppSyncLosesAckedDms` | L3: geACKte berichten in de RAM-queue weg na power cycle; contacten overleven (DataStore op SimFS); klok via contacten |
| `SimCompanion.UnknownCommandGetsErrorFrameViaRawApi` | raw-commando-API; onbekend commando geeft `ERR_CODE_UNSUPPORTED_CMD` |
| `Simulator.*` (13 tests) | determinisme, zendtijd, RTC-semantiek, flash over reboot/wipe, verlies en eenrichtingslinks, uitval midden in een frame, torn write en capaciteit, fast-forward gelijk aan ms-ticks, landing op wakeups, `drop_next`, sync-vlag van de RTC, stroomuitval midden in een schrijfactie, gedeelde volgorde van `fs_log` en `tx_log` |
| `RdmSim.*` (`test_rdm_node`, `native_rdm`) | S01 met twee `RdmSimCompanion`s via een repeater (QUEUED, ON_RADIO, DELIVERED; één `SEND_CONFIRMED`; ACK_R na de inbox-write), zendtijd 2,3 s per hop, standaard Alice (ON_RADIO_FINAL), S02 met en zonder fast-forward byte- en tijdgelijk |

## 6. Wat de simulator al opleverde

- **Hidden terminal na de eerste ACK.** Stuurt Bob direct na de eerste ACK een tweede DM (B-R-A), dan gaat die direct terwijl Alice nog geen pad naar Bob heeft en haar ACK flood stuurt; die botst bij R met Bobs reciprocal PATH (500 ms vertraging, `src/Mesh.cpp:177`). Bericht aangekomen, Bob ziet "failed". Relevant voor de outbox: een time-out na een eerste succes is vaak een verloren ACK, geen verloren bericht.
- **#3518 is reproduceerbaar zonder hardware** en is daarmee de eerste regressietest voor stap 1 uit `03` par. 10.
- **Companion-klok na reboot** komt van contact-`lastmod`, niet van 2024 (par. 4); dit raakt G6 (RDM-klok, "betrouwbaar" alleen na app-sync).
- **Correctie op `01-huidige-werking.md`**: de offline queue is 16 frames in de default, maar 256 op Heltec V3 BLE-builds.

- **ACK_S kan ACK_R inhalen** (WP5). Bij de eerste DM zonder pad zit ACK_R in de PATH-return (prioriteit 2) en gaat ACK_S als flood-ACK (prioriteit 1); synct Alice' app meteen, dan ziet Bob QUEUED en direct DELIVERED. Eindstatus en één `SEND_CONFIRMED` kloppen (G8). ACK_S bewust vertragen maakt het erger: hij botst dan met Bobs reciprocal PATH (500 ms na ontvangst, `src/Mesh.cpp:177`) en Bob wacht op de query na 1 u.

## 7. Afstemming met `06` par. 7

| eis | status |
|---|---|
| S1 `fs()` publiek, `SimFS::files` bewerkbaar | ja; plus `capacity_bytes`, `write_budget`/`fail_writes_after` (ook met `POWER_OFF`), `fs_log` |
| S2 eigen `mesh::Mesh` via `createMesh()` | ja; `beginMesh()`/`loopMesh()` overriden (par. 3) |
| S3 fast-forward | ja: `set_fast_forward`, `busy()`, `nextWakeupMs()`; `RDM_TEST_TIME_DIVISOR` is niet nodig |
| S4 airtime per TX-record | ja: `TxRecord::airtime_ms` en `seq`; `airtime_ms(len)` publiek |
| S5 één specifiek pakket verliezen | ja: `drop_next`, `add_drop_filter` (per transmissie en ontvanger) |
| S6 echte companion `MyMesh` | ja: `CompanionNode`; in `[env:native_rdm]` compileert die met `-D WITH_RELIABLE_DM` mee; RDM-frames via `send_command` en `push_log` |

## 8. Beperkingen

- **Radio**: vaste SNR per link, geen fading, geen capture effect (elke overlap kost beide frames), geen ruisvloer/AGC/CAD-fouten, geen CRC-fouten behalve via `loss`. Alle nodes delen één `RadioConfig`; `CMD_SET_RADIO_PARAMS` op een companion heeft geen effect.
- **Tijd**: resolutie 1 ms; binnen een tick lopen nodes in vaste volgorde. Echte firmware loopt sneller dan 1 kHz en heeft interrupt-timing; races op sub-ms-niveau bestaan hier niet. Fast-forward rekent op `busy()`/`nextWakeupMs()` van eigen nodes; een node met verborgen timers kan tot `max_step` te laat komen.
- **Repeater** is een kale `mesh::Mesh` met forwarding, niet `examples/simple_repeater` (geen regio-filter, ACL, CLI of eigen tx-delay-prefs).
- **ChatNode** is een client op `BaseChatMesh` die companion-defaults nabootst, geen companion. Voor companion-gedrag: `CompanionNode`.
- **CompanionApp** is een model van een standaard app (één commando tegelijk, sync op MSG_WAITING, retrybeleid via `AppRetry`). De echte app is closed source; de open punten in `03` par. 9 blijven HIL.
- **Companion-platform**: gebouwd via het `RP2040_PLATFORM`-codepad met de sim-FS; geen SPIFFS/LittleFS-specifiek gedrag (pagina's, erase, write-cache, `openWrite`-verwijdering op nRF52). Geen UI, geen BLE-MTU of write-busy. Globals (`board`, `radio_driver`, `sensors`) zijn gedeeld door alle companions.
- **Flash** is ideaal: direct duurzaam, geen latentie, geen slijtage; capaciteit telt alleen bytes.
- **Stroomuitval midden in een schrijfactie** (`POWER_OFF`) slaat pas toe na de loop van die node: code na de afgebroken write in dezelfde loop draait nog, maar kan niets meer naar flash schrijven. Een echte crash stopt midden in de functie.
- **Geheugen**: `MyMesh` alloceert per boot een eigen `ArduinoMillis` en pakketpool zonder vrijgave, en `StaticPoolPacketManager` geeft zijn pool niet vrij; per reboot lekt dat in tests. ASan dus met `detect_leaks=0`.
- **Eén `Simulator` per proces tegelijk** (Arduino `millis()`/`random()` zijn globaal).
- Tests hebben bij de eerste build internet nodig voor de lib-downloads.
