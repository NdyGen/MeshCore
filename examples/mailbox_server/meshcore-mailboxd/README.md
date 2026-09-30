# mbxd (Rust)

A drop-in replacement for [`../mbxd/mbxd.py`](../mbxd/mbxd.py): the mailbox daemon on the Pi next to the radio with the `examples/mailbox_server` firmware. Same command line, same line protocol, same SQLite schema: stop `mbxd.py`, start this binary on the same database, and the radio notices nothing. The rules are those of [`../mbxd/README.md`](../mbxd/README.md) and `test/rdm_vectors/mailbox_conformance.json`; this file only covers what is specific to the Rust version.

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

Extending the line protocol (daemon-initiated session, protocol version in HELLO and `mbx.ready`, ACL refresh after `owner-add` without a restart; recommendation 2 in `docs/reliable-dm/09-architectuurreview.md`) touches only the edges: a field on `Request::Hello` and `Reply::Ready` in `protocol`, `Session::hello` becomes the announcement that also runs at start-up, and a new `transport::Event` (for example `Reload` on SIGHUP or a database change) triggers it again. The domain and storage do not change.

## Build and test

```sh
cargo test                                   # unit, conformance vectors, ported test_mbxd.py tests, CLI
cargo clippy --all-targets -- -D warnings
cargo fmt --check
cargo build --release                        # target/release/mbxd
```

Part of `cargo test`:

- `tests/conformance.rs` runs every case of the conformance vectors through the line protocol.
- `tests/session.rs` ports the behaviour tests of `test_mbxd.py`, including a child process that aborts inside SQLite's COMMIT (no row, no reply).
- `tests/cli.rs` drives the binary: the CLI, `serve --stdio [--fake-clock]`, a database created by `mbxd.py` (and handed back to it), and 1800 generated lines through both daemons with identical replies and identical database contents. The tests that need `python3` skip themselves when it is missing.
- `tests/protocol_props.rs`: property tests for the line parser (no panics, damaged fields never parse).

The library denies `unwrap`, `expect` and `panic!` outside tests (clippy) and forbids `unsafe`.

## Raspberry Pi build

The Pi runs 64-bit Raspberry Pi OS (Debian bookworm, glibc 2.36) or the older 32-bit one. Cross-building from macOS with [`cargo-zigbuild`](https://github.com/rust-cross/cargo-zigbuild) works, including the bundled SQLite C code; `.2.28` pins the glibc symbol versions so the binary also runs on bullseye:

```sh
brew install zig
cargo install --locked cargo-zigbuild
rustup target add aarch64-unknown-linux-gnu armv7-unknown-linux-gnueabihf
cargo zigbuild --release --target aarch64-unknown-linux-gnu.2.28      # 64-bit Pi OS (Pi 3/4/5, Zero 2 W)
cargo zigbuild --release --target armv7-unknown-linux-gnueabihf.2.28  # 32-bit Pi OS
```

Output: `target/<target>/release/mbxd`, a stripped 2.5 MB executable that links only glibc (`libc`, `libm`, `libpthread`, `libdl`; symbols up to `GLIBC_2.28`). `cargo` must be rustup's (`~/.cargo/bin` first in `PATH`), not Homebrew's, for the extra targets. Building on the Pi itself (`cargo build --release` with rustup) needs no cross tooling; it was not tried here. [`cross`](https://github.com/cross-rs/cross) is the Docker-based alternative on Linux hosts; it was not needed here.

Checked without a Pi: the aarch64 binary runs in arm64 Linux containers with glibc 2.34 (RHEL 9) and 2.41 (Debian trixie): `owner-add`, `owner-list`, `serve --stdio`, and `serve --port` on a pseudo-terminal (lines split across reads and idle periods, binary noise, a second daemon on the same port refused). On a pty the RTS ioctl fails and is logged, as pyserial ignores it too. Not yet run on a Raspberry Pi with the radio attached.

## Install on the Pi

```sh
sudo useradd --system --no-create-home --groups dialout mbxd
sudo install -d /opt/mbxd && sudo install -m 0755 mbxd /opt/mbxd/
sudo install -m 0644 mbxd.service /etc/systemd/system/     # edit --port first
sudo systemctl daemon-reload && sudo systemctl enable --now mbxd
```

`mbxd.service` in this directory is the unit of `mbxd.py` with the binary in `ExecStart`. Administration is the same as with `mbxd.py`:

```sh
M="sudo -u mbxd /opt/mbxd/mbxd --db /var/lib/mbxd/mbxd.db"
$M owner-add <alice_pub_64hex> [--ttl-days 7] [--sync-days 30] [--quota 20] [--k-owner <32hex>]
$M owner-list
$M deny   <owner4_8hex|owner_pub> <depositor_pub_64hex>
$M undeny <owner4_8hex|owner_pub> <depositor_pub_64hex>
sudo systemctl restart mbxd      # after owner-add: the radio loads owners only after HELLO
```

Switching from `mbxd.py`: `systemctl stop mbxd`, install the binary and this unit, `systemctl start mbxd`. The database needs no migration, and going back is the same steps in reverse.

## Simulator

`test/rdm_support/SubprocessBackend` runs the daemon as `serve --stdio --fake-clock`. With `RDM_MBXD` pointing at this binary (and a `SubprocessBackend` that executes non-`.py` paths directly) `S07_MailboxEchteMbxd` runs against the Rust daemon:

```sh
RDM_MBXD=$PWD/examples/mailbox_server/mbxd-rs/target/release/mbxd \
  pio test -e native_rdm -f test_rdm_scenarios -a "--gtest_filter=*EchteMbxd*"
```

## Differences from mbxd.py

On the wire and in the database there are none known; `tests/cli.rs` compares both daemons reply by reply. The rest:

- Invalid UTF-8 on stdin is debug output (replacement characters); `mbxd.py` in `--stdio` mode stops with a `UnicodeDecodeError`. The serial path of both already replaced it.
- `--fake-clock` is refused with `--port`; `mbxd.py` accepts it and then runs the serial session on a clock stuck at 0.
- Command-line arguments are validated before the database is opened, so a typo does not create an empty database.
- The serial port is opened exclusively (`TIOCEXCL`), so a second daemon on the same port fails instead of sharing it.
- Log lines are `YYYY-MM-DD HH:MM:SS LEVEL message` in UTC (journald adds its own timestamp); `mbxd.py` logs local time with milliseconds.
- An unexpected value in the database (an unknown state name, a blob of the wrong length, limits out of range) is a storage error and answers NO_STORAGE; `mbxd.py` would raise on some of these and exit.

## Dependencies

| crate | why |
|---|---|
| `rusqlite` (`bundled`) | SQLite, compiled in so the Pi build does not depend on the system library version |
| `hmac`, `sha2` | `token_B = HMAC-SHA256(K_owner, sender)[0:8]`, with a constant-time compare |
| `base64` | payloads on the line protocol; configured to accept exactly what Python's strict decoder accepts |
| `getrandom` | a new `K_owner` from the OS, as `os.urandom` |
| `clap` (`derive`) | the command line of `mbxd.py`, including abbreviated long options |
| `serialport` (no default features) | the radio's port; without `libudev`, since `mbxd` never enumerates ports |
| `thiserror` | typed errors per layer |
| `log` | log facade; a 20-line stderr logger in `cli` instead of a logging crate |
| dev: `serde_json`, `tempfile`, `proptest`, `rusqlite/hooks` | vectors, scratch databases, parser properties, commit fault injection |
