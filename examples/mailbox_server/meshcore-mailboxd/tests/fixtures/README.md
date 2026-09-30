# Golden fixtures from mbxd.py

Recorded on 2026-09-30 with the original Python daemon, `examples/mailbox_server/mbxd/mbxd.py` at commit
`10dfcc0e` (Python 3.12.7, SQLite as bundled with it), before that daemon was removed. They pin the behaviour
this daemon replaced; `tests/cli.rs` replays them. Do not regenerate them from this daemon: then they would only
prove that it agrees with itself.

| file | contents |
|---|---|
| `mbxd-py.db` | database after `owner-add` (ttl 3, quota 5; a second owner with defaults), `deny` of Carol for the second owner, and the session `mbxd-py.in` |
| `mbxd-py.in`, `mbxd-py.out` | that session's request lines and mbxd.py's replies (`serve --stdio --fake-clock`) |
| `mbxd-py.owner-list`, `mbxd-py.dump` | mbxd.py's `owner-list` output and the database contents afterwards |
| `diff-<seed>.in` | 600 generated lines per seed: valid, colliding and malformed requests, time jumps up to 31 days, HELLO, an unknown owner; the database had two owners (ttl 7/quota 3 and ttl 2/quota 20) and one denylisted depositor |
| `diff-<seed>.out`, `diff-<seed>.dump` | mbxd.py's replies and the database contents afterwards |

Dump format (`dump()` in `tests/cli.rs`): one line per row, `table: col | col | ...`, blobs as `x'hex'`, text quoted,
`NULL` for null; `owners.created` is left out because the admin commands take it from the real clock. Keys are the
test keys of `tests/common/mod.rs` (`sha256("owner")`, `sha256("k")[..16]`, ...), not secrets.
