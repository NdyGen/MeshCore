# 08: Verificatie v1

Onafhankelijke controle van de v1-implementatie (29 sep 2026), op de hoofdcheckout van `feature/reliable-dm`. Getest op de toen ongecommitte stand; die is tijdens de verificatie gecommit (`1105c812..644ec665`) en is byte-gelijk aan de geteste snapshot. Niets overgenomen uit eerdere rapporten: alles hieronder is zelf gedraaid. Mutaties en ad-hoc tests draaiden in een kopie in de scratchpad.

Bijgewerkt 30 sep 2026: B1, B2, B3 en B5 zijn gedicht met tests in de hoofdcheckout (par. 8), zonder wijziging in productiecode; B6 met twee extra envs in de compat-check. Regelnummers in par. 2-6 verwijzen naar `644ec665`; par. 8 naar de werkmap met de nieuwe tests.

## 1. Tests

`tools/rdm/run-all-tests.sh`, exit 0, `ALL PASSED`:

| env | uitkomst |
|---|---|
| native | 46/46 |
| native_kiss_modem | 8/8 |
| native_sim | 22/22 |
| native_rdm | 431/431 (2 min 52 s; `test_rdm_scenarios` 60 s, `test_rdm_node` 72 s) |
| native_rdm_status | 38/38 |
| pytest mbxd (v1-daemon, in ronde 2 vervangen door `meshcore-mailboxd` met `cargo test`) | 86 passed |

Gelijk aan `06` par. 6.3. Na het dichten van de gaten (par. 8): native_rdm 436/436, de rest ongewijzigd, `ALL PASSED`.

## 2. Traceerbaarheid

Hoe de offline-momenten gemodelleerd zijn:

- **app weg**: de app verbreekt de verbinding, de radio loopt door (`RdmSimCompanion::disconnect`, `test/rdm_support/RdmSimCompanion.cpp:175`; bij `CompanionNode` via companion-frames).
- **radio uit of reboot**: `SimNode::shutdown` gooit mesh, pakketpool, RTC en alle RAM weg, flash blijft (`test/sim/Sim.cpp:151`); zonder RTC-batterij start de klok na boot op 2024 (`Sim.cpp:117`). Dat is een echte reboot, geen pauze.
- **flashverlies**: `/rdm/inbox` en `/rdm/register` gewist terwijl de radio uit is (`test/test_rdm_scenarios/Scenario.cpp:180`); gedeeltelijke corruptie alleen op unitniveau (`InboxTest.TornInboxWriteGivesNoAck`, `test_rdm_storage`).
- **mailbox onbereikbaar**: backend in modus `NO_REPLY` (Pi weg, mailboxradio aan) of `WITHHOLD`.
- **Bobs status**: elk scenario assert de volledige reeks 0x91-pushes (`statuses(id)`), en waar Bobs app weg is de outbox-state in de radio.

### Scenario's uit `05`

Bestand zonder pad: `test/test_rdm_scenarios/test_rdm_scenarios.cpp`.

| # | test(s) | offline-moment | status bij Bob geassert | gat |
|---|---|---|---|---|
| 1 | `RdmSim.S01_Basis` (`test/test_rdm_node/test_rdm_sim.cpp:36`), `RdmSim.S01_FirstDmWithoutPathEndsDelivered` (`:86`), `RdmScenario.S01_Zendtijd` (`:149`) | geen | QUEUED, ON_RADIO, DELIVERED (`test_rdm_sim.cpp:62`); `ACK_R` pas na inbox-write in flash, via volgorde FS-log/TX-log (`:66-80`) | - |
| 2 | `RdmSim.S02_FastForwardGivesTheSameRun` (`test_rdm_sim.cpp:164`), `S02_AliceTelefoonWeg_AckSVerloren` (`:201`), `S02_Zendtijd` (`:168`) | app weg, radio aan, 17,5 u | QUEUED, ON_RADIO, DELIVERED (`:229`); drie queries met ON_RADIO en geldige ack; DELIVERED via query na verloren `ACK_S` (`:226-227`) | - |
| 3 | `S03_AliceRadioUit`, `..._CompanionNode` (`:236`, `:296-297`) | Alice' radio uit vóór verzenden, 5 u | QUEUED, ON_RADIO, DELIVERED (`:289`); app-retries op één entry (`:260`); 1x getoond (`:290`) | levering "binnen één probe-interval" niet direct geasserteerd (B5) |
| 4 | `S04_BobWegVoorAckS`, `..._GevuldRegister` (`:302`, `:347`, `:354`) | Bobs radio en app uit na ON_RADIO, 2 u | outbox DELIVERED zonder app (`:333`), geen 0x91 zonder client (`:334`), reeks na herverbinden (`:339`); variant EVICTED | - |
| 5 | `S05_NooitTegelijk`, `..._SlotNaVierentwintigUur` (`:402`, `:428`) | Alice' link weg, radio 6 u aan / 18 u uit; Bob continu aan (conform `06` par. 6.2) | QUEUED, EXPIRED op 7 d ± 2 s (`:412-413`) | Bob wisselt zelf niet af |
| 6 | `S06_AliceVerliestInbox`, `..._G7Duplicaat` (`:447`, `:485`) | Alice' app weg; inbox en register gewist na `ACK_R` | QUEUED, ON_RADIO, QUEUED, ON_RADIO, DELIVERED (`:477`); 1x getoond; G7-duplicaat vastgepind (`:500`) | - |
| 7 | `S07_Mailbox` (2 topologieën), `..._CompanionNode`, `S07_MailboxEchteMbxd` (`:544`, `:682-694`) | Alice' radio uit bij verzenden, Bob uit in CUSTODY, Alice aan zonder app, app later, Bob aan | QUEUED, CUSTODY, DELIVERED (`:669`); STORED pas na commit (`:609`); Bob controleert `ACK_S` zelf (`:664`) | - |
| 8 | `S08_MailboxResync` (`:698`) | inbox en register gewist na ON_RADIO-rapport; Bob uit | QUEUED, CUSTODY, ON_RADIO, DELIVERED (`:742`); nieuwe `store_id`, andere tag en hash | - |
| 9a | `S09a_MailboxOnbereikbaar` (`:749`) | Pi weg (`NO_REPLY`), Alice' radio 2 u uit | QUEUED, ON_RADIO, DELIVERED, nooit CUSTODY (`:783-784`) | mailboxradio zelf uit alleen op unitniveau (`OutboxTest.G10_SilentOrUnknownFinalStatusExpires`, `test_rdm_outbox.cpp:1374`) |
| 9b | `S09b_MailboxAchterhaald` (`:790`) | M houdt achter (`WITHHOLD`) | QUEUED, CUSTODY, ON_RADIO, DELIVERED (`:826`); DUPLICATE-rapport, 1x getoond | - |
| 10 | `S10_AliceStandaard`, `..._Backoff` (`:833`, `:870`), `RdmSim.StockAliceGetsAnUpstreamAckWithoutCap` (`test_rdm_sim.cpp:135`) | variant: Alice' radio 2 u uit | QUEUED, ON_RADIO_FINAL (`:855`, `:897`); ACK byte-gelijk aan upstream (`:851`); niets meer na FINAL (`:860`) | "standaard" is de fork met `setEnabled(false)`, geen upstream-binary (B4) |

### Requirements

| R | bewijs | oordeel |
|---|---|---|
| R1 standaard app | `S13_StandaardApp`, `..._CompanionNode` (`:1051-1052`, echte `MyMesh` via frames); `RdmCompanion.StandardAppGetsSendConfirmedButNoStatusPushes` (`test/test_rdm_companion_proto/test_companion_mymesh.cpp:156`); `RdmStatusChannel.OnRadioAndDeliveredOnceNoQueuedNoReplayDuplicate` (`:397`) | deels: de app is een testmodel; de drie open punten over de echte standaard app (`03` par. 9) vragen HIL |
| R2 fallback | scenario 10, `S11_CapIngetrokken` (`:902`), `RdmNode.RuntimeOffBehavesLikeUpstream` (`test/test_rdm_node/test_rdm_node.cpp:130`), `InboxTest.StockSenderTextUpToUpstreamMaximumIsStoredAndAcked` (`test/test_rdm_inbox/test_rdm_inbox.cpp:317`), `InboxTest.SenderWithoutCapGetsNoAckS` (`:373`) | deels (B4) |
| R3 E2E | geen test in de repo; ad-hoc bewezen (par. 4) | gat (B3) |
| R4 RF-only | alle scenario's zijn RF-only; `meshcore-mailboxd` heeft geen netwerk-dependency (`examples/mailbox_server/meshcore-mailboxd/Cargo.toml`: seriële poort, SQLite, crypto) | ok, per constructie |
| R5 statussen, onvervalsbaar | statusreeksen in elk scenario; `OutboxTest.G8_CustodyStatusDeliveredWithValidAckS` (`test/test_rdm_outbox/test_rdm_outbox.cpp:1255`), `G4_ProbeSyncedWithValidAckS` (`:757`), `CustodyStatusOnRadioWithValidAckR` (`:1243`), `WrongAckIsIgnored` (`:549`) | gat: vervalste DELIVERED in ON_RADIO onbewaakt (B1) |
| R6 outbox zonder mailbox | S03, S05, `RdmNode.OutboxSurvivesRebootAndLateAckSStillDelivers` (`test_rdm_node.cpp:293`), `OutboxTest.RebootRecomputesAcksAndKeepsFields` (`test_rdm_outbox.cpp:994`), `G6_BootKickRoundAfter60To120sThenSchedule` (`:1016`) | ok; B5 |
| R7 geen host, geen bewaring | S09a; `MailboxCoreTest.NoBackendAnswerWithinThreeSecondsIsNoStorage` (`test/test_rdm_mailbox_core/test_mailbox_core.cpp:301`), `BackendNotReadyAnswersNoStorageAtOnce` (`:358`); `crash_between_insert_and_commit_gives_no_reply_and_no_row` (`meshcore-mailboxd/tests/session.rs`) | ok |
| R8 identiek zonder flag | par. 5 | ok; B6 |

## 3. Mutaties

20 mutaties in kernlogica, elk in een eigen kopie, gevolgd door de volledige `pio test -e native_rdm` (en, sinds ronde 2, `cargo test` voor `meshcore-mailboxd`; in v1 pytest voor `mbxd`). Nulmeting op de onbewerkte kopie: 431/431. Script en patronen: `scratchpad/mut/mutations.py` en `run1.sh` van deze sessie.

| # | mutatie | uitkomst | gedood door (selectie) |
|---|---|---|---|
| M01 | `ACK_R` besloten vóór de inbox-write, dus ook bij INBOX_FULL/STORE_ERROR (`RdmInbox.cpp:405-407`) | gedood (7) | `InboxTest.InboxFullGivesNoAck`, `RdmNode.AckOnlyForStoredMessages` |
| M02 | mislukte inbox-write genegeerd (`RdmInbox.cpp:353`) | **overleeft** | - (B2) |
| M03 | `ACK_S` bij ontvangst i.p.v. bij sync | gedood (16) | `S01_Zendtijd`, `S02_*`, `S04_*`, `S06_*`, `S14`, `InboxTest.NewMessageIsPersistedBeforeAckAndOfferedToApp` |
| M04 | sync bij overhandigen i.p.v. bij volgend `CMD_SYNC_NEXT_MESSAGE` | gedood (3) | `InboxTest.SyncedOnlyByTheNextSyncRequest`, `LostConnectionReoffersTheMessage` |
| M05 | geen dedup op het register | gedood (11) | `S09b`, `S14`, `RdmNode.LostAckIsResentAndShownOnce` |
| M06 | ongesyncte registerregels mogen worden verwijderd | gedood (3) | `InboxTest.EvictsOnlySyncedLowestTimestampAndSetsWatermark` |
| M07 | geen inboxquotum per afzender | gedood (4) | `InboxTest.QuotaIsAQuarterOfTheInboxPerSender` |
| M08 | outbox niet persistent | gedood (11) | `S04_*`, `S07_*`, `S08`, `RdmNode.OutboxSurvivesReboot...` |
| M09 | STATUS DELIVERED in CUSTODY zonder `ACK_S`-check | gedood (1) | `OutboxTest.G8_CustodyStatusDeliveredWithValidAckS` |
| M10 | STATUS DELIVERED in ON_RADIO zonder `ACK_S`-check (`RdmOutbox.cpp:768`) | **overleeft** | - (B1) |
| M11 | query SYNCED zonder `ACK_S`-check | gedood (1) | `OutboxTest.G4_ProbeSyncedWithValidAckS` |
| M12 | EVICTED betekent DELIVERED ook vóór ON_RADIO | gedood (1) | `OutboxTest.ProbeEvictedBeforeOnRadioSendsDm` |
| M13 | geen G17-jitter in de outbox | gedood (3) | `OutboxTest.G17_TwoNodesStartingTogetherDoNotStayInStep` |
| M14 | geen G17-jitter op het FETCH-interval | gedood (9) | `FetcherTest.G17_TwoNodesOnTheSameSecondDrift`, alle mailboxscenario's |
| M15 | eerste RETRY-actie/boot-kick direct i.p.v. na 60-120 s | gedood (27) | `OutboxTest.G6_BootKick...`, `G17_*`, scenario's |
| M16 | T_radio/T_sync verlopen nooit | gedood (11) | `S05_NooitTegelijk`, `OutboxTest.TRadioExpiresRetryExactly` |
| M17 | cap nooit ingetrokken (G15) | gedood (2) | `S11_CapIngetrokken` |
| M18 | mailboxradio antwoordt STORED vóór commit van de Pi | gedood (11) | `MailboxCoreTest.NoBackendAnswerWithinThreeSecondsIsNoStorage`, `S07_*` |
| M19 | de daemon (`meshcore-mailboxd`) zet DELIVERED op een ON_RADIO-rapport | gedood (v1: pytest 11, native 2) | `S07_MailboxEchteMbxd`, conformance-vectoren |
| M20 | reconcile laat een ongesyncte registerregel zonder inboxrecord staan | gedood (1) | `InboxTest.LostInboxDropsUnsyncedRegisterRowsSoTheSenderResends` |

18 van 20 gedood. De twee overlevers zijn geen fouten in de code (die is correct), maar ongeteste garanties. Beide zijn in de kopie met een gerichte test gedood (par. 4 en B2).

## 4. E2E-claim (R3, R5)

In de repo bestaat geen test die bewijst dat de mailbox de plaintext niet kan lezen, en geen scenario met een liegende mailbox; alleen unit tests op Bobs ack-controle (par. 2). Daarom twee ad-hoc tests toegevoegd in een kopie (`scratchpad/mut/test_verify_e2e.cpp`, plus een modus `lie` in `MemMailboxBackend`):

- **`Verify.E2E_MailboxSeesOnlyCiphertext`** (echte `meshcore-mailboxd`): na DEPOSIT bevat geen enkel pakket van of naar M, ontsleuteld met M's eigen sleutels, de tekst (23 pakketten); de gedeelde sleutels M-Bob en M-Alice openen de inner payload niet, Bob-Alice wel (positieve controle); de SQLite-bestanden van de daemon bevatten de ciphertext wel en de plaintext niet. **Geslaagd.**
- **`Verify.E2E_LyingMailboxCannotForgeDelivered`**: M beantwoordt elke STATUS met DELIVERED, eerst met de `ACK_R` uit Alice' rapport (de sterkste leugen die M kan maken), daarna met willekeurige bytes; 12 leugens over 4 dagen. Bob blijft op QUEUED, CUSTODY, ON_RADIO; pas na Alice' echte sync volgt DELIVERED. **Geslaagd.** Tegen mutant M10 faalt deze test: Bob toont DELIVERED terwijl Alice' app niets heeft (`bob 0x91 [QUEUED, CUSTODY, ON_RADIO, DELIVERED], alice app shows 0x`).

Conclusie: de claim klopt voor v1, maar de repo bewijst hem niet.

## 5. Compat (R8)

`tools/rdm/check-upstream-identical.sh --compare-only` op de bestaande build (23:18-23:20, geen bron nieuwer): CODE-IDENTICAL, exit 0. Omdat bestanden bij integratie met oude mtime gekopieerd kunnen zijn, daarna ook volledig herbouwd (`--work` in de scratchpad): eveneens CODE-IDENTICAL, exit 0. RAK 4631, native en native_kiss_modem byte-identiek; Heltec V3 en T-Beam alleen debug-info in `main.cpp.o`, `MyMesh.cpp.o`, `UITask.cpp.o`, `BaseChatMesh.cpp.o`, `Mesh.cpp.o` en de ELF/image-hashes in `firmware.bin`.

## 6. Bevindingen

Status per bevinding: par. 8.

| # | ernst | bevinding | aanbeveling |
|---|---|---|---|
| B1 | middel | De `ACK_S`-controle op STATUS DELIVERED in ON_RADIO (`src/helpers/rdm/RdmOutbox.cpp:768`) is door geen test bewaakt (M10 overleeft). Een regressie daar laat een kwaadwillende mailbox "afgeleverd" vervalsen, precies wat R5 uitsluit; aangetoond in par. 4. Het CUSTODY-geval heeft wel een test (`test_rdm_outbox.cpp:1255`). | Unit test naast `G8_CustodyStatusDeliveredWithValidAckS` voor ON_RADIO, en de liegende-mailboxtest uit par. 4 in `test_rdm_scenarios` opnemen. |
| B2 | middel | De controle op een mislukte inbox-write (`src/helpers/rdm/RdmInbox.cpp:353`) is ongetest (M02 overleeft). De schrijffouttests modelleren stroomuitval (alle latere writes falen, `test/rdm_support/MemFileIO.h:24`) of alleen een registerfout (`test_rdm_inbox.cpp:304`); daardoor faalt de registerwrite altijd mee. Dit is de kern van "`ACK_R` pas na persistente opslag" (#3518). | Test met `io.fail_path = INBOX_PATH`; in de kopie geschreven: slaagt op de huidige code, faalt op M02. |
| B3 | laag | R3 heeft geen test in de repo; de eigenschap volgt uit de constructie (inner payload onder ECDH Bob-Alice) en is ad-hoc bevestigd. | `Verify.E2E_MailboxSeesOnlyCiphertext` opnemen. |
| B4 | laag | "Standaard firmware" is in alle tests de fork met `Node::setEnabled(false)`, niet upstream-code; standaard Bob naar fork-Alice alleen op unitniveau. De sim-repeater is een kale `Mesh` met `allowPacketForward` (`test/sim/ChatNode.cpp:272`); de forwardcode zelf is ongewijzigd (de `Mesh.cpp`-diff raakt alleen `createDatagram`). | In HIL (`06` par. 8) een echte upstream-companion en -repeater meenemen. |
| B5 | laag | Scenario's draaien grotendeels op `RdmSimCompanion`; de echte `MyMesh` alleen in S03, S07 en S13. De sim-app synct altijd door tot NO_MORE_MESSAGES (`RdmSimCompanion.cpp:195`), wat voor de standaard app een open punt is (`03` par. 9). De R6-toets "afgeleverd binnen één probe-interval" is niet direct geasserteerd: S03 begrenst op 2 u (`test_rdm_scenarios.cpp:270`) en toetst de intervallen alleen vóór het aangaan (`:274`). Geen scenario met Bobs reboot in RETRY (wel unit, `test_rdm_outbox.cpp:994`, `:1016`). | Assert in S03 dat de eerste probe na aangaan binnen 30 min + jitter valt; S04 ook als `_CompanionNode`. |
| B6 | info | `check-upstream-identical.sh` vergelijkt alleen de companion-envs en native (`tools/rdm/check-upstream-identical.sh:18`); repeater-, room- en sensorbuilds compileren `Mesh.cpp` ook maar worden niet vergeleken. Zonder flag is identiek gedrag daar te verwachten, niet bewezen. | Eén repeater-env aan `ENVS` toevoegen. |

## 7. Eindoordeel

In de simulator werkt v1 aantoonbaar. Alle tests slagen (436/436 native_rdm). De scenario's 1-10 modelleren de offline-momenten realistisch (echte reboot met RAM-verlies, app weg, flashverlies, Pi weg) en asserten de volledige statusreeks bij Bob. Na par. 8 worden alle 20 gerichte mutaties gevangen. De E2E-eigenschappen (M ziet geen plaintext, kan "afgeleverd" niet vervalsen) staan nu als scenario in de repo. Zonder flag is de build code-identiek aan upstream.

Niet aangetoond: gedrag op echte radio's, met een echte upstream-companion en -repeater (B4) en met de echte standaard app (R1, `03` par. 9). Dat is HIL (`06` par. 8), de volgende stap.

## 8. Gedichte gaten (30 sep 2026)

Alleen tests en testsupport gewijzigd. Bewijs tegen de mutanten: de bijgewerkte hoofdcheckout gekopieerd, mutant toegepast met hetzelfde patroon als in par. 3, de betrokken suites gedraaid.

| # | status | test(s) | bewijs |
|---|---|---|---|
| B1 | opgelost | `OutboxTest.OnRadioStatusDeliveredWithoutValidAckSIsIgnored` (`test/test_rdm_outbox/test_rdm_outbox.cpp:1270`): in ON_RADIO worden DELIVERED met `ACK_R` en met willekeurige bytes genegeerd, met `ACK_S` geaccepteerd. `RdmScenario.E2E_LyingMailboxCannotForgeDelivered` (`test/test_rdm_scenarios/test_rdm_scenarios.cpp:1205`): M liegt 4 dagen DELIVERED (eerst met `ACK_R`, dan willekeurig) via `MemMailboxBackend::setLie` (`test/rdm_support/MemMailboxBackend.h:21`, `.cpp:314`); Bob blijft op CUSTODY, ON_RADIO tot Alice echt synct. | Op M10 falen beide; op de huidige code slagen beide. |
| B2 | opgelost | `InboxTest.InboxWriteFailureAloneGivesNoAck` (`test/test_rdm_inbox/test_rdm_inbox.cpp:304`): alleen de inbox-write faalt (`io.fail_path = INBOX_PATH`); geen `ACK_R`, geen spoor, query UNKNOWN, later wel NEW. | Op M02 faalt de test; op de huidige code slaagt hij. |
| B3 | opgelost | `RdmScenario.E2E_MailboxSeesOnlyCiphertext` (`test_rdm_scenarios.cpp:1161`), met echte `meshcore-mailboxd`: geen plaintext in wat M met eigen sleutels ontsleutelt, M-Bob en M-Alice openen de inner payload niet, de database van de daemon bevat ciphertext maar geen plaintext. | Er is in v1 geen codepad dat de plaintext naar M brengt, dus geen natuurlijke mutant. Positieve controles (Bob-Alice opent de payload; de ciphertext staat in de database) voorkomen dat de test vacuüm slaagt. |
| B4 | open | - | Vraagt HIL. |
| B5 | opgelost (deels) | S03 assert nu dat de eerste probe na aangaan het probeschema voortzet en binnen 30 min + jitter na aangaan valt (`test_rdm_scenarios.cpp:286-291`); daarna volgt de DM binnen 60 s. `RdmScenario.S04_BobWegVoorAckS_CompanionNode` (`:363`): S04 op de echte `MyMesh`. | Beide slagen op beide companionmodellen. Open blijft het punt over de sync-lus van de standaard app (`03` par. 9, HIL); een scenario met Bobs reboot in RETRY is niet toegevoegd (unit-dekking volstaat, par. 2). |
| B6 | opgelost | `tools/rdm/check-upstream-identical.sh` vergelijkt nu ook `Heltec_v3_repeater` en `Heltec_v3_room_server` (`ENVS`, aangepast door een andere sessie). | `--compare-only`: CODE-IDENTICAL, exit 0. Repeater: obj 288, pp 4, img 1; room server: obj 287, pp 4, img 1; 0 verschil in code of data. Alleen debug-info in `Mesh.cpp.o` en `BaseChatMesh.cpp.o`, plus de ELF/image-hashes in `firmware.bin`. Door deze verificatie nagedraaid met hetzelfde resultaat. |

`tools/rdm/run-all-tests.sh` na deze wijzigingen: native 46/46, native_kiss_modem 8/8, native_sim 22/22, native_rdm 436/436, native_rdm_status 38/38, pytest `mbxd` 86 passed (v1-daemon, sindsdien vervangen door `meshcore-mailboxd`), `ALL PASSED`. `tools/rdm/check-headers.py`: 0 ontbrekend.
