# meshcore-mailboxd: mailbox daemon

`meshcore-mailboxd` runs on the mailbox's own Raspberry Pi, next to a radio with the `examples/mailbox_server` firmware. The radio does the crypto, the peer cache and the airtime; the daemon is the source of truth for the deposited messages, the owners, the depositor registrations, the tokens and the denylist. It never sees plaintext: a deposit is the complete encrypted TXT_MSG payload, stored bit for bit.

Design: `docs/reliable-dm/03-ontwerp.md` (par. 3, 4, 7 and 11). Line protocol, session protocol v2 and interfaces: `docs/reliable-dm/06-implementatieplan-v1.md` par. 3.15-3.17. This README pins down what those leave open and is normative for the daemon's behaviour; `test/rdm_vectors/mailbox_conformance.json` fixes the operations for both this daemon and the C++ reference `test/rdm_support/MemMailboxBackend`, and `test/rdm_vectors/mbxd_session.json` fixes the session lines for this daemon and the radio's `SerialPiBackend`. The daemon replaced a Python implementation (`mbxd.py`) with the same operations, command line and database; `tests/fixtures/` keeps what that one did.

## Install on the Pi

Build the binary for the Pi (see "Raspberry Pi build") or on the Pi itself, then:

```sh
sudo useradd --system --no-create-home --groups dialout meshcore-mailboxd
sudo install -d /opt/meshcore-mailboxd && sudo install -m 0755 meshcore-mailboxd /opt/meshcore-mailboxd/
sudo install -m 0644 meshcore-mailboxd.service /etc/systemd/system/     # edit --port first
sudo systemctl daemon-reload && sudo systemctl enable --now meshcore-mailboxd
```

The serial port is opened once and held. The daemon then sends `mbx.hello?`, and the radio answers with `@MBX HELLO` whether it just booted (opening the port resets the ESP32) or was already running; either way the session is set up (see "Session"). The database lives in `/var/lib/meshcore-mailboxd/mailbox.db` (`StateDirectory` of the unit, `--db` on the command line); a database from `mbxd.py` works as it is, whatever its path.

`meshcore-mailboxd.service` is the unit of `mbxd.py` with the binary in `ExecStart`: `Restart=always` with 5 s, `ProtectSystem=strict`, `ProtectHome`, `PrivateTmp`, `NoNewPrivileges`. The daemon needs no network; adding `PrivateNetwork=yes` and a `DeviceAllow` for the port is possible but has not been tried on a Pi.

## Owners and depositors

```sh
M="sudo -u meshcore-mailboxd /opt/meshcore-mailboxd/meshcore-mailboxd --db /var/lib/meshcore-mailboxd/mailbox.db"
$M owner-add <alice_pub_64hex> [--ttl-days 7] [--sync-days 30] [--quota 20] [--k-owner <32hex>]
$M owner-list
$M deny   <owner4_8hex|owner_pub> <depositor_pub_64hex>
$M undeny <owner4_8hex|owner_pub> <depositor_pub_64hex>
```

No restart is needed: the running daemon notices the change within a second and sends the radio its ACL again (see "Session"). `owner-add` prints `K_owner`. Alice enters it, together with the mailbox's public key, in her app (`CMD_RDM_SET_MAILBOX`); her radio derives a `token_B = HMAC-SHA256(K_owner, pub_Bob)[0:8]` per favourite contact and sends it in MBX_INFO. Running `owner-add` again changes the limits and keeps the key unless `--k-owner` is given. Owners are identified by their 4-byte prefix on the wire, so two owners with the same prefix are refused. `deny` also drops the depositor's registration; after `undeny` the depositor has to register again.

Limits: `ttl_days` 1-30 (T_radio), `sync_days` 1-255 (T_sync), `quota` 1-255 live messages per depositor.

Command line: `--db <path>` and `-v`/`--verbose` go before the subcommand; long options may be abbreviated (`--ttl 5`). Exit code 2 for a wrong argument, an unknown owner or a prefix collision (message `meshcore-mailboxd: ...` on stderr), 1 for anything else, such as an unwritable database. Logging goes to stderr as `YYYY-MM-DD HH:MM:SS LEVEL message` (UTC; journald adds its own timestamp), INFO by default, DEBUG with `-v`.

## Line protocol details

115200 baud, one request per line, strictly in order. Formats are in 06 par. 3.16. In addition:

- Lines that do not start with `@MBX ` are debug output and ignored, as are lines with an unknown command. A malformed `@MBX` line (field count, more than one space between fields, hex length, base64, more than 8 reports or hashes, a request id that is not 1-10 digits within 32 bits) gets no reply and is logged; the radio's 3 s timeout turns that into NO_STORAGE. Hex is accepted in either case; base64 must be canonical (padded, standard alphabet). A trailing `\r` is ignored. Invalid UTF-8 counts as debug output. A partial serial line longer than 4096 bytes is dropped.
- A reply is written only after its transaction is committed (WAL, `synchronous=FULL`). If the commit fails, the reply carries code `13` (NO_STORAGE) and nothing is changed (`mbx.stat` then carries `-`). A crash before the commit leaves no row and no reply.
- `@MBX HELLO 2 [fw]` is answered with `mbx.acl` lines, `mbx.ready 2` and `mbx.time <unix>`: all owners first (role `o`, owner4 = own prefix), then depositors by most recent registration or deposit (role `d`), one line per public key, denylisted ones left out, 64 lines at most. A HELLO with another protocol version gets `mbx.ready 2` alone; see "Session". If the ACL cannot be read, the daemon exits (systemd restarts it, and the restart makes it ask `mbx.hello?` again).
- `mbx.time <unix>` repeats every 600 s after the `mbx.time` that followed a HELLO (never before one). The radio extrapolates it with `millis()` and uses it for `ttl_s` in the DEPOSIT reply (`expires - time`) and for `tijd_M` in the registration reply, so Bob never needs M's clock to match his own. On the serial port a due `mbx.time` also goes out after 1 s of silence.
- `mbx.fetch`: `resterend` is the number of STORED copies after this reply (capped at 255); the payload field is `-` without a copy. `mbx.stat` lists `state:ack` per requested hash; a non-OK code carries `-`.
- The daemon never answers RATE_LIMITED; rate limits live in `MailboxCore` on the radio.
- The request id is echoed as a decimal number (leading zeros dropped); codes are two lowercase hex digits.

## Session

Session protocol v2 (06 par. 3.16, `test/rdm_vectors/mbxd_session.json`). `PROTO` = 2 is `protocol::PROTO`.

- At start, before reading any input, the daemon sends `mbx.hello?` once. It is not repeated: a radio that boots later sends its own HELLO, and a radio that missed it (the port open reset it) boots into a HELLO as well.
- `@MBX HELLO <proto> [<fw>]`: the first field is the protocol version when it is a decimal number (1-10 digits); the rest, if any, is the firmware version and is only logged. With `proto` = 2 the answer is the ACL series, `mbx.ready 2` and `mbx.time <unix>`, and the `mbx.time` beacon (re)starts. Every HELLO gets this, also a second one while the radio is already ready (its answer to `mbx.hello?` after a daemon restart, or a boot HELLO next to it).
- Version mismatch: a HELLO with another version, or the v1 form `@MBX HELLO [<fw>]` whose first field is not decimal (an empty HELLO, `@MBX HELLO 1.0-rdm`, `@MBX HELLO sim`), is logged as one WARN line per HELLO (a v1 radio repeats it every 30 s) and answered with `mbx.ready 2` alone: no ACL, no `mbx.time`, and the beacon does not start. The radio then shows which version it is talking to and waits for the next `mbx.hello?`. A later `@MBX HELLO 2` gets the full series.
- Requests (STORE, REG, FETCH, STAT) are answered whatever the session state, before any HELLO as well. It is the radio that holds them back until it is ready.
- ACL change without a restart: after `owner-add`, `deny` or `undeny` in another process, the running daemon sends the ACL series and `mbx.ready 2` again, without `mbx.time`, within about a second. The radio takes the series idempotently. The daemon's own changes to the ACL (a registration through REG) are not pushed: the radio just talked to that peer, and the next HELLO lists it.

How the daemon notices the change: it polls SQLite's `PRAGMA data_version` on its own connection, at most once per second (`transport::ACL_POLL`, driven by the idle event the quiet input yields every second, on the serial port and on stdin alike). That counter moves only when another connection commits, not on the daemon's own commits, not on read-only transactions such as `owner-list`, and not on a rolled back one, in WAL mode too; `storage::Storage::changed_elsewhere` wraps it, and a change that a HELLO's series already carried is taken off the check first. The alternatives were weighed and left out:

- A signal from the command line (SIGHUP, `systemctl reload`): the admin command would have to find the daemon's pid (a pid file next to the database, or systemd's `MAINPID`), which does not exist in the stdio mode the C++ scenarios use, and it needs the same user as the daemon; a change made with any other tool (`sqlite3`) would go unnoticed. The signal would also have to interrupt a blocking read, which needs a self-pipe or a signal crate.
- `inotify` on the database: in WAL mode a commit lands in the `-wal` file, and a checkpoint moves it later; both would fire, none says whether the ACL changed, and it is Linux only.
- Both would only add to the poll, not replace it: the poll costs one pragma per second (a read of the WAL index header, no disk access) and covers every writer.

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

Timers run at the start of every request, at the request's time. A final state counts its 30 days from the moment the timer ran out (not from when the daemon noticed), or from the report that made it final.

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

`owners(pubkey, owner4, k_owner, ttl_days, sync_days, quota, store_id, created, last_sender)`, `acl(pubkey, owner4, sent_count, created, last_seen)`, `denylist(owner4, pubkey, created)`, `messages(pkt_hash, owner4, sender, payload, created, t_radio, t_sync, state, ack_r, ack_s, final_at)`. Compared with the sketch in 03 par. 7: owner prefix and `store_id` per owner, `last_seen` for the HELLO order, `final_at` for the 30-day retention and `last_sender` for the FETCH rotation (added to an older database on open). The schema is `storage::SCHEMA`, unchanged since `mbxd.py`; states are stored as `stored`, `sent`, `on_radio`, `delivered`, `rejected`, `expired`, `sync_expired`. A value the daemon cannot interpret (unknown state, blob of the wrong length, limits out of range) is a storage error and answers NO_STORAGE.

## Tests

```sh
cargo test                                   # unit, conformance vectors, behaviour, CLI, fixtures
cargo clippy --all-targets -- -D warnings
cargo fmt --check
pio test -e native_rdm -f test_rdm_mailbox_conformance   # the same vectors against MemMailboxBackend
```

Both run every case in `test/rdm_vectors/mailbox_conformance.json`; a change to the rules changes the vectors and both implementations. Part of `cargo test`:

- `tests/conformance.rs` runs every case of the vectors through the line protocol (request lines built independently of the crate's formatter).
- `tests/session.rs` covers the line handling, commit-before-reply, a child process that aborts inside SQLite's COMMIT (no row, no reply), a failed commit (NO_STORAGE, nothing changed), restart with `store_id`, the rotation over a restart, an older database without `last_sender`, administration, the session protocol (`mbx.hello?`, HELLO with and without the right version, requests in any session state, the ACL push after a change by another connection) and `mbx.time`.
- `tests/session_vectors.rs` plays the daemon side of every case in `test/rdm_vectors/mbxd_session.json` whose `players` include `pi` (7 cases) against the binary with `serve --stdio --fake-clock`, the admin commands running in a second process while it serves; the radio side of the same file is `test/test_rdm_mailbox_core/test_serial_pi_backend.cpp`.
- `tests/cli.rs` drives the binary: the command line and exit codes, `serve --stdio [--fake-clock]`, `serve --port` on a pseudo-terminal with the ACL push after `owner-add`, `deny` and `undeny`, and the golden fixtures in `tests/fixtures/` recorded with `mbxd.py` before it was removed: a database it wrote (opened, continued, resynced), and three 600-line transcripts whose replies and resulting database contents must match line for line. `tests/fixtures/README.md` says how they were made and how their session lines were transcribed to protocol v2.
- `tests/protocol_props.rs`: property tests for the line parser (no panics on any input, damaged fields never parse).

The library forbids `unsafe` and denies `unwrap`, `expect` and `panic!` outside tests (clippy).

## Simulator

For the simulator (`test/rdm_support/SubprocessBackend`), the daemon runs on stdin/stdout with a clock that follows the simulation:

```sh
meshcore-mailboxd --db /tmp/sim.db owner-add <pub> --k-owner <hex>
meshcore-mailboxd --db /tmp/sim.db serve --stdio --fake-clock
```

With `--fake-clock` the line `@MBX TIME <unix>` sets the clock (it starts at 0); it gets no reply of its own, but a due `mbx.time` goes out right after it. Without the flag that line is ignored like any unknown command. `--fake-clock` is refused with `--port`. On stdin/stdout the session is the same as on the port: `mbx.hello?` is the first line on stdout, a HELLO needs the version (`@MBX HELLO 2 sim`), and an admin command in another process gets an ACL push within a second. With `RDM_MBXD` pointing at the binary, `S07_MailboxEchteMbxd` runs against it once `test/rdm_support/SubprocessBackend` speaks v2 (it wraps `SerialPiBackend` for that):

```sh
RDM_MBXD=$PWD/examples/mailbox_server/meshcore-mailboxd/target/release/meshcore-mailboxd \
  pio test -e native_rdm -f test_rdm_scenarios -a "--gtest_filter=*EchteMbxd*"
```

## Layers

Functional core, imperative shell. Dependencies point inwards; only the outer ring does I/O.

| module | role | I/O |
|---|---|---|
| `types` | byte values (`Pubkey`, `Owner4`, `PktHash`, `Ack`, ...), `Code`, `State`, `Report` | none |
| `protocol` | parse `@MBX` lines into a typed `Request`, format `Reply` lines | none |
| `domain::model`, `domain::policy` | owners, limits, messages; token check, timers, report transitions, resync, sender rotation, STATUS visibility, ACL order | none, time is an argument |
| `domain::usecases` | STORE, REG, FETCH, STAT, ACL, `owner-add`, `deny` against a `storage::Repo` | through the trait |
| `storage` | `Storage` / `Transaction` / `Repo` traits; `SqliteStorage` | SQLite |
| `mailbox` | one transaction per operation, returns after the commit | through the trait |
| `session` | line in, replies out; NO_STORAGE on a failed commit; `mbx.time` beacon | none (clock injected) |
| `system`, `transport`, `cli` | clocks, randomness, stdin/stdout, serial port, command line | yes |

A reply is formatted only from what `Mailbox` returns, and `Mailbox` returns only after `commit()`, so there is no path that answers before the data is durable.

Session protocol v2 (recommendation 2 in `docs/reliable-dm/09-architectuurreview.md`) lives at the edges: the version field on `Request::Hello` and `Reply::Ready` and the `Reply::HelloQuery` line in `protocol`; `Session::start` (the `mbx.hello?`), the version check in `Session::hello` and `Session::refresh_acl` (the push) in `session`; the change check `Storage::changed_elsewhere` in `storage`; the poll cadence and the stdin thread in `transport`. The domain did not change.

## Build

```sh
cargo build --release                        # target/release/meshcore-mailboxd
```

Rust 1.88 or newer (`rust-version` in `Cargo.toml`).

### Raspberry Pi build

The Pi runs 64-bit Raspberry Pi OS (Debian bookworm, glibc 2.36) or the older 32-bit one. Cross-building from macOS with [`cargo-zigbuild`](https://github.com/rust-cross/cargo-zigbuild) works, including the bundled SQLite C code; `.2.28` pins the glibc symbol versions so the binary also runs on bullseye:

```sh
brew install zig
cargo install --locked cargo-zigbuild
rustup target add aarch64-unknown-linux-gnu armv7-unknown-linux-gnueabihf
cargo zigbuild --release --target aarch64-unknown-linux-gnu.2.28      # 64-bit Pi OS (Pi 3/4/5, Zero 2 W)
cargo zigbuild --release --target armv7-unknown-linux-gnueabihf.2.28  # 32-bit Pi OS
```

Output: `target/<target>/release/meshcore-mailboxd`, a stripped 2.5 MB executable that links only glibc (`libc`, `libm`, `libpthread`, `libdl`; symbols up to `GLIBC_2.28`). `cargo` must be rustup's (`~/.cargo/bin` first in `PATH`), not Homebrew's, for the extra targets. Building on the Pi itself (`cargo build --release` with rustup) needs no cross tooling; it was not tried here. [`cross`](https://github.com/cross-rs/cross) is the Docker-based alternative on Linux hosts; it was not needed here.

Checked without a Pi: the aarch64 binary runs in arm64 Linux containers with glibc 2.34 (RHEL 9) and 2.41 (Debian trixie): `owner-add`, `owner-list`, `serve --stdio`, and `serve --port` on a pseudo-terminal (lines split across reads and idle periods, binary noise, a second daemon on the same port refused). On a pty the RTS ioctl fails and is logged, as pyserial ignored it too. Not yet run on a Raspberry Pi with the radio attached.

## Differences from mbxd.py

On the wire and in the database there are none known; `tests/cli.rs` replays the recorded fixtures reply by reply. The rest:

- Invalid UTF-8 on stdin is debug output (replacement characters); `mbxd.py` in `--stdio` mode stopped with a `UnicodeDecodeError`. The serial path of both already replaced it.
- `--fake-clock` is refused with `--port`; `mbxd.py` accepted it and then ran the serial session on a clock stuck at 0.
- Command-line arguments are validated before the database is opened, so a typo does not create an empty database.
- The serial port is opened exclusively (`TIOCEXCL`), so a second daemon on the same port fails instead of sharing it.
- Log lines are in UTC without milliseconds; `mbxd.py` logged local time with milliseconds.
- An unexpected value in the database is a storage error and answers NO_STORAGE; `mbxd.py` raised on some of these and exited.
- The default database path is `/var/lib/meshcore-mailboxd/mailbox.db` instead of `/var/lib/mbxd/mbxd.db`; pass `--db` to keep using an existing file.
- Session protocol v2 (see "Session"); `mbxd.py` spoke v1: no `mbx.hello?`, a bare `mbx.ready`, every HELLO answered with the full series, and a restart after `owner-add`. A radio with v1 firmware gets `mbx.ready 2` alone from this daemon and does not become ready; a radio with v2 firmware got a bare `mbx.ready` from `mbxd.py` and did not become ready either. The operations and their replies are unchanged, which is why the recorded fixtures still apply after transcribing their session lines.

## Dependencies

| crate | why |
|---|---|
| `rusqlite` (`bundled`) | SQLite, compiled in so the Pi build does not depend on the system library version |
| `hmac`, `sha2` | `token_B = HMAC-SHA256(K_owner, sender)[0:8]`, with a constant-time compare |
| `base64` | payloads on the line protocol; configured to accept exactly canonical, padded base64 |
| `getrandom` | a new `K_owner` from the OS |
| `clap` (`derive`) | the command line, including abbreviated long options |
| `serialport` (no default features) | the radio's port; without `libudev`, since the daemon never enumerates ports |
| `thiserror` | typed errors per layer |
| `log` | log facade; a 20-line stderr logger in `cli` instead of a logging crate |
| dev: `serde_json`, `tempfile`, `proptest`, `rusqlite/hooks`, `nix` | vectors, scratch databases, parser properties, commit fault injection, a pseudo-terminal for the `serve --port` test |
