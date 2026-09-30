//! The `meshcore-mailboxd` binary as the simulator and the Pi admin use it: command line, `serve --stdio
//! [--fake-clock]`, and the golden fixtures recorded with the original Python daemon (`tests/fixtures/README.md`):
//! a database it wrote, and line transcripts with its replies and the database contents they left behind.

mod common;

use std::io::{BufRead, BufReader, Write};
use std::path::{Path, PathBuf};
use std::process::{Command, Output, Stdio};
use std::sync::mpsc;
use std::time::Duration;

use common::*;

fn daemon(db: &Path, args: &[&str], stdin: Option<&str>) -> Output {
    run(Command::new(daemon_bin()), db, args, stdin)
}

fn run(mut cmd: Command, db: &Path, args: &[&str], stdin: Option<&str>) -> Output {
    let mut child = cmd
        .arg("--db")
        .arg(db)
        .args(args)
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .unwrap();
    let mut input = child.stdin.take().unwrap();
    input
        .write_all(stdin.unwrap_or_default().as_bytes())
        .unwrap();
    drop(input);
    child.wait_with_output().unwrap()
}

fn stdout(o: &Output) -> String {
    String::from_utf8_lossy(&o.stdout).into_owned()
}

fn ok(o: Output) -> String {
    assert!(
        o.status.success(),
        "exit {:?}: {}",
        o.status,
        String::from_utf8_lossy(&o.stderr)
    );
    stdout(&o)
}

#[test]
fn cli_owner_add_list_and_deny() {
    let dir = tempfile::tempdir().unwrap();
    let db = dir.path().join("cli.db");
    let out = ok(daemon(
        &db,
        &[
            "owner-add",
            &hex(&owner()),
            "--ttl-days",
            "5",
            "--quota",
            "9",
        ],
        None,
    ));
    let k = out.split("k_owner ").nth(1).unwrap().trim();
    assert_eq!(out, format!("owner {} k_owner {k}\n", o4(&owner())));
    assert_eq!(unhex(k).len(), 16);
    let list = ok(daemon(&db, &["owner-list"], None));
    assert_eq!(
        list,
        format!(
            "owner {} owner4={} ttl_days=5 sync_days=30 quota=9 store_id=-\n",
            hex(&owner()),
            o4(&owner())
        )
    );
    assert!(!list.contains(k), "listing does not print secrets");
    let again = ok(daemon(
        &db,
        &[
            "owner-add",
            &hex(&owner()).to_uppercase(),
            "--sync-days",
            "3",
        ],
        None,
    ));
    assert!(
        again.ends_with(&format!("k_owner {k}\n")),
        "keeps the key: {again}"
    );
    assert!(ok(daemon(&db, &["deny", &o4(&owner()), &hex(&bob())], None)).is_empty());
    assert!(ok(daemon(&db, &["undeny", &hex(&owner()), &hex(&bob())], None)).is_empty());
    let unknown = daemon(&db, &["deny", "ffffffff", &hex(&bob())], None);
    assert_eq!(unknown.status.code(), Some(2));
    assert_eq!(
        String::from_utf8_lossy(&unknown.stderr),
        "meshcore-mailboxd: unknown owner ffffffff\n"
    );
}

#[test]
fn cli_rejects_bad_arguments_with_exit_2() {
    let dir = tempfile::tempdir().unwrap();
    let db = dir.path().join("bad.db");
    let pk = hex(&owner());
    let mut other = owner();
    other[4..].fill(0);
    assert!(daemon(&db, &["owner-add", &pk], None).status.success());
    let cases: Vec<Vec<String>> = vec![
        vec!["owner-add".into(), "abcd".into()],
        vec!["owner-add".into(), "zz".repeat(32)],
        vec![
            "owner-add".into(),
            pk.clone(),
            "--ttl-days".into(),
            "0".into(),
        ],
        vec![
            "owner-add".into(),
            pk.clone(),
            "--ttl-days".into(),
            "31".into(),
        ],
        vec![
            "owner-add".into(),
            pk.clone(),
            "--quota".into(),
            "256".into(),
        ],
        vec![
            "owner-add".into(),
            pk.clone(),
            "--sync-days".into(),
            "-1".into(),
        ],
        vec![
            "owner-add".into(),
            pk.clone(),
            "--k-owner".into(),
            "00".repeat(15),
        ],
        vec!["owner-add".into(), hex(&other)],
        vec!["deny".into(), "abc".into(), hex(&bob())],
        vec!["deny".into(), o4(&owner()), "00".into()],
        vec!["serve".into()],
        vec![
            "serve".into(),
            "--stdio".into(),
            "--port".into(),
            "/dev/null".into(),
        ],
        vec![
            "serve".into(),
            "--port".into(),
            "/dev/null".into(),
            "--fake-clock".into(),
        ],
        vec!["nope".into()],
    ];
    for args in cases {
        let args: Vec<&str> = args.iter().map(String::as_str).collect();
        let out = daemon(&db, &args, None);
        assert_eq!(
            out.status.code(),
            Some(2),
            "{args:?}: {}",
            String::from_utf8_lossy(&out.stderr)
        );
    }
    let limits = daemon(&db, &["owner-add", &pk, "--ttl-days", "0"], None);
    assert_eq!(
        String::from_utf8_lossy(&limits.stderr),
        "meshcore-mailboxd: ttl_days 1-30, sync_days 1-255, quota 1-255\n"
    );
}

#[test]
fn stdio_mode_with_fake_clock() {
    let dir = tempfile::tempdir().unwrap();
    let db = dir.path().join("stdio.db");
    ok(daemon(
        &db,
        &["owner-add", &hex(&owner()), "--k-owner", &hex(&k_owner())],
        None,
    ));
    let o = o4(&owner());
    let lines = [
        "@MBX HELLO test".to_string(),
        format!("@MBX TIME {T0}"),
        reg_line(1, &hex(&bob()), &o, &hex(&token(&bob(), &k_owner()))),
        store_line(2, &o, &hex(&bob()), &"07".repeat(8), &[b'z'; 20]),
        format!("@MBX TIME {}", T0 + 7 * DAY),
        stat_line(3, &hex(&bob()), &[&"07".repeat(8)]),
    ];
    let out = ok(daemon(
        &db,
        &["serve", "--stdio", "--fake-clock"],
        Some(&(lines.join("\n") + "\n")),
    ));
    let want = [
        format!("mbx.acl {} o {o}", hex(&owner())),
        "mbx.ready".into(),
        "mbx.time 0".into(),
        format!("mbx.time {T0}"),
        "mbx.reg 1 00 7 20".into(),
        format!("mbx.store 2 00 {}", T0 + 7 * DAY),
        format!("mbx.time {}", T0 + 7 * DAY),
        format!("mbx.stat 3 00 5:{}", "00".repeat(6)),
    ];
    assert_eq!(out.lines().collect::<Vec<_>>(), want);
}

#[test]
fn time_line_ignored_without_fake_clock() {
    let dir = tempfile::tempdir().unwrap();
    let out = ok(daemon(
        &dir.path().join("t.db"),
        &["serve", "--stdio"],
        Some("@MBX TIME 5\n@MBX HELLO\n"),
    ));
    let lines: Vec<&str> = out.lines().collect();
    assert_eq!((lines[0], lines.len()), ("mbx.ready", 2));
    let t: i64 = lines[1].strip_prefix("mbx.time ").unwrap().parse().unwrap();
    let now = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .unwrap()
        .as_secs();
    assert!(
        (t - i64::try_from(now).unwrap()).abs() < 60,
        "real clock, not the ignored TIME line"
    );
}

#[test]
fn stdio_answers_each_line_before_eof() {
    let dir = tempfile::tempdir().unwrap();
    let mut child = Command::new(daemon_bin())
        .arg("--db")
        .arg(dir.path().join("live.db"))
        .args(["serve", "--stdio"])
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::null())
        .spawn()
        .unwrap();
    let mut input = child.stdin.take().unwrap();
    let (tx, rx) = mpsc::channel();
    let output = BufReader::new(child.stdout.take().unwrap());
    std::thread::spawn(move || {
        output
            .lines()
            .map_while(Result::ok)
            .for_each(|l| tx.send(l).unwrap_or(()))
    });
    let next = || {
        rx.recv_timeout(Duration::from_secs(10))
            .expect("no reply within 10 s")
    };

    writeln!(input, "@MBX HELLO").unwrap();
    assert_eq!(next(), "mbx.ready");
    assert!(next().starts_with("mbx.time "));
    writeln!(
        input,
        "{}",
        stat_line(1, &hex(&bob()), &["01".repeat(8).as_str()])
    )
    .unwrap();
    assert_eq!(next(), format!("mbx.stat 1 00 0:{}", "00".repeat(6)));
    drop(input);
    assert!(child.wait().unwrap().success());
}

#[test]
fn invalid_utf8_on_stdin_is_debug_output() {
    let dir = tempfile::tempdir().unwrap();
    let mut child = Command::new(daemon_bin())
        .arg("--db")
        .arg(dir.path().join("u.db"))
        .args(["serve", "--stdio", "--fake-clock"])
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::null())
        .spawn()
        .unwrap();
    let mut input = child.stdin.take().unwrap();
    input.write_all(b"\xff\xfe garbage\n@MBX HELLO\n").unwrap();
    drop(input);
    let out = child.wait_with_output().unwrap();
    assert!(out.status.success());
    assert_eq!(
        String::from_utf8_lossy(&out.stdout),
        "mbx.ready\nmbx.time 0\n"
    );
}

/// Everything in the database except `owners.created`, which each CLI run takes from the real clock.
fn dump(db: &Path) -> Vec<String> {
    let c = rusqlite::Connection::open(db).unwrap();
    let mut out = Vec::new();
    for (table, cols) in [
        (
            "owners",
            "pubkey, owner4, k_owner, ttl_days, sync_days, quota, store_id, last_sender",
        ),
        ("acl", "pubkey, owner4, sent_count, created, last_seen"),
        ("denylist", "owner4, pubkey"),
        (
            "messages",
            "pkt_hash, owner4, sender, payload, created, t_radio, t_sync, state, ack_r, ack_s, final_at",
        ),
    ] {
        let mut stmt = c
            .prepare(&format!("SELECT {cols} FROM {table} ORDER BY rowid"))
            .unwrap();
        let n = stmt.column_count();
        let rows = stmt
            .query_map([], |r| {
                let vals: Vec<String> = (0..n)
                    .map(|i| match r.get_ref(i).unwrap() {
                        rusqlite::types::ValueRef::Null => "NULL".to_string(),
                        rusqlite::types::ValueRef::Integer(v) => v.to_string(),
                        rusqlite::types::ValueRef::Real(v) => v.to_string(),
                        rusqlite::types::ValueRef::Text(t) => {
                            format!("'{}'", String::from_utf8_lossy(t))
                        }
                        rusqlite::types::ValueRef::Blob(b) => format!("x'{}'", hex(b)),
                    })
                    .collect();
                Ok(format!("{table}: {}", vals.join(" | ")))
            })
            .unwrap();
        out.extend(rows.map(Result::unwrap));
    }
    out
}

fn fixture(name: &str) -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR"))
        .join("tests/fixtures")
        .join(name)
}

fn fixture_text(name: &str) -> String {
    std::fs::read_to_string(fixture(name)).unwrap()
}

fn fixture_lines(name: &str) -> Vec<String> {
    fixture_text(name).lines().map(String::from).collect()
}

/// The owners and denylist entry the compatibility fixture was recorded with.
fn compat_admin(db: &Path) {
    ok(daemon(
        db,
        &[
            "owner-add",
            &hex(&owner()),
            "--k-owner",
            &hex(&k_owner()),
            "--ttl-days",
            "3",
            "--quota",
            "5",
        ],
        None,
    ));
    ok(daemon(
        db,
        &[
            "owner-add",
            &hex(&sha256(b"owner2")),
            "--k-owner",
            &hex(&k_owner()),
        ],
        None,
    ));
    ok(daemon(
        db,
        &["deny", &o4(&sha256(b"owner2")), &hex(&carol())],
        None,
    ));
}

#[test]
fn uses_a_database_made_by_mbxd_py() {
    let dir = tempfile::tempdir().unwrap();
    let db = dir.path().join("py.db");
    std::fs::copy(fixture("mbxd-py.db"), &db).unwrap();
    assert_eq!(
        dump(&db),
        fixture_lines("mbxd-py.dump"),
        "the fixture is what was recorded"
    );
    let o = o4(&owner());
    let (b, c) = (hex(&bob()), hex(&carol()));
    let owner2 = sha256(b"owner2");
    let bob_first = bob() < carol();

    // the Rust daemon picks up where mbxd.py stopped: registrations, rotation, store_id, ON_RADIO with ACK_R
    assert_eq!(
        ok(daemon(&db, &["owner-list"], None)),
        fixture_text("mbxd-py.owner-list")
    );
    let rs_lines = [
        format!("@MBX TIME {}", T0 + 60),
        stat_line(1, &b, &[&"01".repeat(8), &"03".repeat(8)]),
        fetch_line(2, &hex(&owner()), 0, "0000abcd", &[]),
        fetch_line(
            3,
            &hex(&owner()),
            0,
            "0000abcd",
            &[(&"02".repeat(8), 1, &"bb".repeat(6))],
        ),
        reg_line(4, &c, &o4(&owner2), &hex(&token(&carol(), &k_owner()))),
        fetch_line(5, &hex(&owner()), 2, "00001111", &[]),
        "@MBX HELLO".to_string(),
        format!("@MBX TIME {}", T0 + 120),
        stat_line(9, &c, &[&"02".repeat(8)]),
    ];
    let out = ok(daemon(
        &db,
        &["serve", "--stdio", "--fake-clock"],
        Some(&(rs_lines.join("\n") + "\n")),
    ));
    let second_copy = if bob_first {
        b"from carol".to_vec()
    } else {
        b"from bob, second".to_vec()
    };
    let lines: Vec<&str> = out.lines().collect();
    assert_eq!(
        lines[0],
        format!("mbx.stat 1 00 2:{},1:{}", "aa".repeat(6), "00".repeat(6))
    );
    assert_eq!(
        lines[1],
        format!("mbx.fetch 2 00 1 0 {}", b64(&second_copy)),
        "rotation continues after mbxd.py's copy"
    );
    assert!(lines[2].starts_with("mbx.fetch 3 00 "), "{}", lines[2]);
    assert_eq!(lines[3], "mbx.reg 4 10 0 0", "denylist from mbxd.py");
    assert!(
        lines[4].starts_with("mbx.fetch 5 00 "),
        "resync against mbxd.py's store_id: {}",
        lines[4]
    );
    assert_eq!(lines[5], format!("mbx.acl {} o {o}", hex(&owner())));
    assert_eq!(
        lines[6],
        format!("mbx.acl {} o {}", hex(&owner2), o4(&owner2))
    );
    assert_eq!(
        lines[7..11],
        [
            format!("mbx.acl {c} d {o}"),
            format!("mbx.acl {b} d {o}"),
            "mbx.ready".into(),
            format!("mbx.time {}", T0 + 60)
        ]
    );
    assert_eq!(
        lines[11],
        format!("mbx.stat 9 00 3:{}", "bb".repeat(6)),
        "DELIVERED with ACK_S"
    );
    assert_eq!(lines.len(), 12);
}

#[test]
fn replays_the_recorded_session_that_made_the_fixture_database() {
    let dir = tempfile::tempdir().unwrap();
    let db = dir.path().join("replay.db");
    compat_admin(&db);
    let out = ok(daemon(
        &db,
        &["serve", "--stdio", "--fake-clock"],
        Some(&fixture_text("mbxd-py.in")),
    ));
    assert_eq!(out, fixture_text("mbxd-py.out"));
    assert_eq!(dump(&db), fixture_lines("mbxd-py.dump"));
}

/// The transcripts were generated as a deterministic mix of valid, colliding and malformed lines (registrations
/// with wrong tokens, deposits of 1 to 165 bytes, FETCHes with random reports and store_ids, STATs, time jumps up
/// to 31 days, HELLO, an unknown owner), enough variety to reach every rule.
#[test]
fn same_replies_and_database_as_mbxd_py() {
    let dir = tempfile::tempdir().unwrap();
    let mut codes = std::collections::BTreeSet::new();
    for seed in [1u64, 2, 3] {
        let db = dir.path().join(format!("rs{seed}.db"));
        for (pk, ttl, quota) in [(owner(), "7", "3"), (sha256(b"owner2"), "2", "20")] {
            let args = [
                "owner-add",
                &hex(&pk),
                "--k-owner",
                &hex(&k_owner()),
                "--ttl-days",
                ttl,
                "--quota",
                quota,
            ];
            ok(daemon(&db, &args, None));
        }
        ok(daemon(
            &db,
            &["deny", &o4(&owner()), &hex(&sha256(b"s3"))],
            None,
        ));
        let input = fixture_text(&format!("diff-{seed}.in"));
        assert!(input.lines().count() >= 600);
        let rs = ok(daemon(
            &db,
            &["serve", "--stdio", "--fake-clock"],
            Some(&input),
        ));
        let py = fixture_lines(&format!("diff-{seed}.out"));
        let rs: Vec<&str> = rs.lines().collect();
        assert!(py.len() > 300, "seed {seed}: only {} replies", py.len());
        for (i, (p, r)) in py.iter().zip(&rs).enumerate() {
            assert_eq!(*r, p, "seed {seed}, reply {i}");
        }
        assert_eq!(rs.len(), py.len(), "seed {seed}");
        assert_eq!(
            dump(&db),
            fixture_lines(&format!("diff-{seed}.dump")),
            "seed {seed}: database contents"
        );
        let replies = py
            .iter()
            .filter(|l| !l.starts_with("mbx.time") && !l.starts_with("mbx.acl"));
        codes.extend(
            replies
                .filter_map(|l| l.split(' ').nth(2))
                .map(String::from),
        );
    }
    for c in ["00", "01", "10", "11", "12", "14"] {
        assert!(codes.contains(c), "code {c} never exercised: {codes:?}");
    }
}
