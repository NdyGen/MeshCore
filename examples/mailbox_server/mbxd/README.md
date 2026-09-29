# mbxd: mailbox daemon

`mbxd` runs on the mailbox's own Raspberry Pi, next to a radio with the `examples/mailbox_server` firmware. The radio does the crypto, the peer cache and the airtime; `mbxd` is the source of truth for the deposited messages, the owners, the depositor registrations, the tokens and the denylist. It never sees plaintext: a deposit is the complete encrypted TXT_MSG payload, stored bit for bit.

Design: `docs/reliable-dm/03-ontwerp.md` (par. 3, 4, 7 and 11). Line protocol and interfaces: `docs/reliable-dm/06-implementatieplan-v1.md` par. 3.15-3.17. This README pins down what those leave open; `test/rdm_vectors/mailbox_conformance.json` fixes the behaviour for both `mbxd` and the C++ reference `test/rdm_support/MemMailboxBackend`.

## Install on the Pi

```sh
sudo apt install python3-serial
sudo useradd --system --no-create-home --groups dialout mbxd
sudo install -d /opt/mbxd && sudo install -m 0755 mbxd.py /opt/mbxd/
sudo install -m 0644 mbxd.service /etc/systemd/system/     # edit --port first
sudo systemctl daemon-reload && sudo systemctl enable --now mbxd
```

The serial port is opened once and held: every open resets the ESP32, which then sends `@MBX HELLO`.

## Owners and depositors

```sh
M="sudo -u mbxd python3 /opt/mbxd/mbxd.py --db /var/lib/mbxd/mbxd.db"
$M owner-add <alice_pub_64hex> [--ttl-days 7] [--sync-days 30] [--quota 20] [--k-owner <32hex>]
$M owner-list
$M deny   <owner4_8hex|owner_pub> <depositor_pub_64hex>
$M undeny <owner4_8hex|owner_pub> <depositor_pub_64hex>
sudo systemctl restart mbxd      # after owner-add: the radio loads owners only after HELLO
```

`owner-add` prints `K_owner`. Alice enters it, together with the mailbox's public key, in her app (`CMD_RDM_SET_MAILBOX`); her radio derives a `token_B = HMAC-SHA256(K_owner, pub_Bob)[0:8]` per favourite contact and sends it in MBX_INFO. Running `owner-add` again changes the limits and keeps the key unless `--k-owner` is given. Owners are identified by their 4-byte prefix on the wire, so two owners with the same prefix are refused. `deny` also drops the depositor's registration; after `undeny` the depositor has to register again.

Limits: `ttl_days` 1-30 (T_radio), `sync_days` 1-255 (T_sync), `quota` 1-255 live messages per depositor.

## Line protocol details

115200 baud, one request per line, strictly in order. Formats are in 06 par. 3.16. In addition:

- Lines that do not start with `@MBX ` are debug output and ignored. A malformed `@MBX` line (field count, hex length, base64, more than 8 reports or hashes) gets no reply and is logged; the radio's 3 s timeout turns that into NO_STORAGE.
- A reply is written only after its transaction is committed (WAL, `synchronous=FULL`). If the commit fails, the reply carries code `13` (NO_STORAGE) and nothing is changed. A crash before the commit leaves no row and no reply.
- `@MBX HELLO [fw]` is answered with `mbx.acl` lines, `mbx.ready` and `mbx.time <unix>`: all owners first (role `o`, owner4 = own prefix), then depositors by most recent registration or deposit (role `d`), one line per public key, denylisted ones left out, 64 lines at most.
- `mbx.time <unix>` repeats every 600 s after HELLO (never before). The radio extrapolates it with `millis()` and uses it for `ttl_s` in the DEPOSIT reply (`expires - time`) and for `tijd_M` in the registration reply, so Bob never needs M's clock to match his own.
- `mbx.fetch`: `resterend` is the number of STORED copies after this reply (capped at 255); the payload field is `-` without a copy. `mbx.stat` lists `state:ack` per requested hash; a non-OK code carries `-`.
- `mbxd` never answers RATE_LIMITED; rate limits live in `MailboxCore` on the radio.

## Message states

`SENT` is internal: a copy went out in a FETCH reply and waits for the next FETCH. On the wire (STATUS) it reads as STORED.

| state | wire | payload | leaves the state by |
|---|---|---|---|
| STORED | 1 | kept | FETCH hands it out (SENT); report; `t_radio` (EXPIRED) |
| SENT | 1 | kept | report; next FETCH without a report for it (STORED, offered again in its sender's turn); `t_radio` (EXPIRED) |
| ON_RADIO | 2 + ACK_R | kept | SYNCED report (DELIVERED); other `store_id` (STORED, new `t_radio`); `t_sync` (SYNC_EXPIRED) |
| DELIVERED | 3 + ACK_S | wiped | removed 30 days later |
| REJECTED | 4 | wiped | removed 30 days later |
| EXPIRED | 5 | wiped | removed 30 days after `t_radio` |
| SYNC_EXPIRED | 6 + ACK_R | wiped | SYNCED report (DELIVERED); removed 30 days after `t_sync` |

Timers run at the start of every request, at the request's time. A final state counts its 30 days from the moment the timer ran out (not from when `mbxd` noticed), or from the report that made it final.

## Rules per request

**REG** (`sender`, `owner4`, `token`): unknown owner: UNKNOWN_OWNER. Denylisted, or `token != HMAC-SHA256(K_owner, sender)[0:8]`: NOT_AUTH. Otherwise OK with the owner's `ttl_days` and `quota`, and the depositor is registered for that owner. Failures carry `ttl` and `quota` 0.

**STORE** (`owner4`, `sender`, `pkt_hash`, `payload`), checked in this order:

1. payload above 164 bytes: TOO_BIG
2. unknown owner: UNKNOWN_OWNER
3. denylisted, or not registered for this owner: NOT_AUTH
4. `pkt_hash` already known: ALREADY_STORED with the original `expires` if owner and sender match, else NOT_AUTH
5. live messages (STORED, SENT, ON_RADIO) of this sender for this owner at `quota`: QUOTA
6. OK: STORED, `expires = t_radio = now + ttl_days`

Non-OK codes carry `expires` 0.

**FETCH** (`client`, `flags`, `store_id`, reports): only an owner's own public key is answered, anything else gets NOT_AUTH. Then, in one transaction:

1. `store_id` differs from the one in the owner's previous FETCH: ON_RADIO and SENT go back to STORED with a new `t_radio = now + ttl_days` (RESYNC, G12: T_radio is about reaching Alice's radio, which had already happened). DELIVERED and other final states stay. The first FETCH only records `store_id`.
2. Reports, in order. All of them count in `rapporten_ok`, including reports for unknown hashes, for another owner's messages and ones that do not fit the state (those change nothing), so Alice can drop them.

   | report | from | to |
   |---|---|---|
   | ON_RADIO, DUPLICATE | STORED, SENT | ON_RADIO, keeps the ack as ACK_R, `t_sync = now + sync_days` |
   | SYNCED | STORED, SENT, ON_RADIO, SYNC_EXPIRED | DELIVERED, keeps the ack as ACK_S |
   | UNDECRYPTABLE | STORED, SENT | REJECTED |
   | INBOX_FULL | SENT | STORED |

3. Every SENT copy that is still SENT goes back to STORED: the reply that carried it did not arrive.
4. Without `NO_PAYLOAD` (0x02) one STORED copy becomes SENT and goes in the reply. Senders take turns: the next sender in pubkey order (bytewise) after the sender of the previous copy handed out to this owner, wrapping around to the lowest; of that sender the oldest copy (deposit time, then arrival). One sender with a full quota therefore does not hold up the others. A copy that went back to STORED in step 3 comes again when its sender is next. A FETCH without a copy does not move the rotation; the rotation survives restarts.

**STAT** (`sender`, hashes): per hash the state of that sender's own message, with ACK_R for ON_RADIO and SYNC_EXPIRED, ACK_S for DELIVERED, zeros otherwise. Unknown hashes and other senders' messages read as UNKNOWN (0).

## Database

`owners(pubkey, owner4, k_owner, ttl_days, sync_days, quota, store_id, created, last_sender)`, `acl(pubkey, owner4, sent_count, created, last_seen)`, `denylist(owner4, pubkey, created)`, `messages(pkt_hash, owner4, sender, payload, created, t_radio, t_sync, state, ack_r, ack_s, final_at)`. Compared with the sketch in 03 par. 7: owner prefix and `store_id` per owner, `last_seen` for the HELLO order, `final_at` for the 30-day retention and `last_sender` for the FETCH rotation (added to an older database on open).

## Tests

```sh
python3 -m pytest examples/mailbox_server/mbxd
pio test -e native_rdm -f test_rdm_mailbox_conformance
```

Both run every case in `test/rdm_vectors/mailbox_conformance.json`; a change to the rules changes the vectors and both implementations. `test_mbxd.py` also covers the line parser, commit-before-reply, a crash between insert and commit, restart and the CLI.

For the simulator (`SubprocessBackend`), `mbxd` runs on stdin/stdout with a clock that follows the simulation:

```sh
python3 mbxd.py --db /tmp/sim.db owner-add <pub> --k-owner <hex>
python3 mbxd.py --db /tmp/sim.db serve --stdio --fake-clock
```

With `--fake-clock` the line `@MBX TIME <unix>` sets the clock (it starts at 0); it gets no reply of its own, but a due `mbx.time` goes out right after it. In serial mode `mbxd` also sends a due `mbx.time` when the radio is silent. Without the flag that line is ignored like any unknown command.
