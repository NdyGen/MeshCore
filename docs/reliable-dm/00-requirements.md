# 00: Requirements betrouwbare DM's

Door Andy bevestigd op 29 sep 2026; R5 en R7 aangescherpt met de beslissingen van dezelfde dag (`03-ontwerp.md` par. 9), R6-toets aangescherpt na review I10 (`04-review.md`). "Bob" is de afzender, "Alice" de ontvanger, "M" een mailbox.

## Doel

Bob en Alice kunnen elkaar DM's sturen met gegarandeerde aflevering, los van of de ander op dat moment bereikbaar is. Bob ziet op elk moment in welke toestand zijn bericht is.

## Requirements

| # | requirement | toets |
|---|---|---|
| R1 | Bob en Alice draaien een geforkte companion-firmware; de telefoon-app mag de standaard MeshCore-app blijven. | Dagelijks gebruik (versturen, lezen, "afgeleverd") werkt in de standaard app. Tussenstatussen via `PUSH_CODE_RDM_STATUS` in een eigen client, of in de standaard app via het optionele lokale statuskanaal "rdm-status". |
| R2 | Graceful fallback: met standaard-nodes gewoon normale DM's; extra garanties alleen als beide kanten de fork draaien. Oude nodes mogen niet breken. | Standaard repeaters forwarden alle nieuwe verkeer; standaard companions tonen of negeren het zonder fout. |
| R3 | End-to-end: een tussenstation of mailbox kan de inhoud niet lezen. Een gewone room server valt daarmee af. | M ziet alleen ciphertext en metadata (pubkeys, grootte, tijdstip). |
| R4 | RF-only moet werken; beperkte reikwijdte door hop count of flood-scope is acceptabel. Internet/MQTT-koppeling tussen mailboxen is nice-to-have en mag niet botsen met de AUP van dutchmeshcore.nl (geen "bridge-achtige constructies"). | Volledige keten werkt zonder internet. |
| R5 | Bob ziet "in bewaring" (een mailbox heeft het bericht duurzaam opgeslagen), "op Alice' radio" (persistent opgeslagen, telefoon nog niet gesynct) en "afgeleverd" (Alice' telefoon heeft het opgehaald). De laatste twee zijn end-to-end bewijzen van Alice die mailbox noch repeater kan vervalsen. Draait Alice standaard firmware, dan is "op Alice' radio" het maximum. | Een kwaadwillende M kan hooguit "in bewaring" liegen of berichten achterhouden. |
| R6 | Werkt ook zonder mailbox: dan een persistente outbox bij Bob met retries. | Bericht overleeft reboot van Bobs radio. Is Alice' radio bereikbaar, dan wordt het afgeleverd binnen één probe-interval (Alice met fork, `03` par. 4) of bij de volgende DM-retry (Alice standaard). Gemeten in stap 4 met een radio die periodiek aan staat. |
| R7 | Mailbox-opslag is in praktijk onbeperkt via een host: een dedicated mailbox met eigen Heltec en eigen Pi (daemon `mbxd`, codebasis rs01d), los van RS01. De radionode is transport. | Zonder host geen "in bewaring". |
| R8 | Eigen gebruik nu, later een upstream-PR: optionele feature achter build-flag, nette protocolextensie. | Zonder flag bit-identiek gedrag aan upstream. |

## Twee soorten "offline"

| situatie | wat er nu gebeurt (`01-huidige-werking.md`) | wat het ontwerp moet doen |
|---|---|---|
| Alice' telefoon weg, radio aan | Radio ontvangt en ACKt; bericht staat in RAM-queue. Verloren bij volle queue (#3518, L2) of reboot (L3). Bob ziet al "delivered". | Radio slaat persistent op en stuurt pas dan de radio-ACK ("op Alice' radio"). De eind-ACK ("afgeleverd") volgt pas als de telefoon het bericht synct. Alice' radio bewaart het tot die sync, zonder TTL. |
| Alice' radio uit of buiten bereik | Niemand bewaart het (L1). | Outbox bij Bob (R6) of mailbox (R5/R7). |
| Bobs telefoon weg na versturen | Radio verstuurt, maar retries zijn app-logica (L5). | Outbox in de radio loopt door zonder app; statuswijzigingen worden bewaard tot de app terugkomt. |
| Bobs radio uit na versturen | Pending state weg. | Outbox in flash, hervat na boot. Bewijs "afgeleverd" wacht bij M of komt via een latere ACK of een `RECEIPT_QUERY` aan Alice. |

## Fysieke grens

- **Zonder mailbox**: aflevering kan alleen als Bobs en Alice' radio ooit tegelijk aan zijn en elkaar via RF (eventueel via repeaters) bereiken. Geen protocol lost dat op; de outbox vergroot alleen de kans dat zo'n moment benut wordt.
- **Met mailbox**: Bob moet M een keer bereiken en Alice moet M een keer bereiken, niet tegelijk. Voor de status "afgeleverd" moet Alice' telefoon daarna met haar radio verbinden, en moet Bob nog een keer M of Alice bereiken.
- **"Afgeleverd" hangt van Alice af**: opent ze haar app niet, dan blijft Bob op "op Alice' radio" staan. Na T_sync (default 30 dagen) toont Bob "niet gesynct"; een later bewijs zet de status alsnog op "afgeleverd".
- **Alice moet Bob als contact hebben** (anders kan ze niet ontsleutelen, L10). Dit is een voorwaarde, geen garantie die het protocol kan geven; het ontwerp maakt het wel zichtbaar als status.

## Buiten scope

- Groepsberichten (channels), room-posts.
- Forward secrecy of verbergen van metadata (wie met wie praat) voor M.
- Mailbox-naar-mailbox synchronisatie (niet nodig bij een ontvanger-gekozen mailbox, zie `03-ontwerp.md`).
