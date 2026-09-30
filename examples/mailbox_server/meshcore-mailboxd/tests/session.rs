//! The behaviour tests of `test_mbxd.py` that pin down semantics beyond the vectors: line handling, durability,
//! restart, administration, HELLO and `mbx.time`.

mod common;

use common::*;
use meshcore_mailboxd::domain::model::Limits;
use meshcore_mailboxd::mailbox::{AdminError, Mailbox};
use meshcore_mailboxd::storage::SqliteStorage;
use meshcore_mailboxd::transport::{Event, serve};
use meshcore_mailboxd::types::{KOwner, Owner4, Pubkey, State};

/// One `serve` run is one daemon start: it opens with `mbx.hello?`, which is checked and left out here.
fn run(s: &mut TestSession, events: Vec<Option<String>>) -> Vec<String> {
    let mut out = Vec::new();
    let events = events
        .into_iter()
        .map(|e| Ok(e.map_or(Event::Idle, Event::Line)));
    serve(s, events, &mut out).unwrap();
    let mut lines: Vec<String> = String::from_utf8(out)
        .unwrap()
        .lines()
        .map(String::from)
        .collect();
    assert_eq!(lines.first().map(String::as_str), Some("mbx.hello?"));
    lines.remove(0);
    lines
}

fn line(l: impl Into<String>) -> Option<String> {
    Some(l.into())
}

fn zeros(n: usize) -> String {
    "00".repeat(n)
}

// ---- line parser ---------------------------------------------------------------------------------------

#[test]
fn debug_output_and_blank_lines_ignored() {
    let fx = Fixture::new();
    let mut s = fx.session();
    for l in [
        "",
        "MBX STORE 1",
        "mbx.store 1 00 5",
        "12:00:00 - 27/9/2026 U RAW: 0102",
        "@MBXSTORE 1",
        "@MBX",
    ] {
        assert!(lines(&mut s, l).is_empty(), "{l:?}");
    }
}

#[test]
fn malformed_lines_get_no_reply() {
    let fx = Fixture::new();
    let mut s = fx.session();
    registered(&mut s, &bob(), Limits::default());
    let (z32, z8) = (zeros(32), zeros(8));
    let nine_reports: Vec<String> = (0..9).map(|i| format!("{i:016x}:0:{}", zeros(6))).collect();
    let nine_hashes: Vec<String> = (0..9).map(|i| format!("{i:016x}")).collect();
    let bad = [
        "@MBX".to_string(),
        "@MBX NOPE 1".into(),
        "@MBX STORE 1 aabbccdd".into(),
        format!("@MBX STORE x aabbccdd {z32} {z8} AAAA"),
        format!("@MBX STORE 4294967296 aabbccdd {z32} {z8} AAAA"),
        format!("@MBX STORE -1 aabbccdd {z32} {z8} AAAA"),
        format!("@MBX STORE 1 aabbcc {z32} {z8} AAAA"),
        format!("@MBX STORE 1 aabbccdd {} {z8} AAAA", zeros(31)),
        format!("@MBX STORE 1 aabbccdd {} {z8} AAAA", "zz".repeat(32)),
        format!("@MBX STORE 1 aabbccdd {z32} {z8} AAA"),
        format!("@MBX STORE 1 aabbccdd {z32} {z8} A*AA"),
        format!("@MBX STORE 1 aabbccdd {z32} {z8} "),
        format!("@MBX STORE 1  aabbccdd {z32} {z8} AAAA"),
        format!("@MBX STORE 1 aabbccdd {z32} {z8} AAAA extra"),
        format!("@MBX REG 1 {z32} aabbccdd {}", zeros(7)),
        format!("@MBX FETCH 1 {z32} 0 00000001 -"),
        format!("@MBX FETCH 1 {z32} 00 0001 -"),
        format!("@MBX FETCH 1 {z32} 00 00000001 "),
        format!("@MBX FETCH 1 {z32} 00 00000001 {z8}:0"),
        format!("@MBX FETCH 1 {z32} 00 00000001 {z8}:x:{}", zeros(6)),
        format!("@MBX FETCH 1 {z32} 00 00000001 {z8}:256:{}", zeros(6)),
        format!("@MBX FETCH 1 {z32} 00 00000001 {}", nine_reports.join(",")),
        format!("@MBX STAT 1 {z32}"),
        format!("@MBX STAT 1 {z32} -"),
        format!("@MBX STAT 1 {z32} {}", nine_hashes.join(",")),
        format!("@MBX STAT 1 {z32} {z8},"),
        "@MBX TIME 5".into(),
    ];
    for l in &bad {
        assert!(lines(&mut s, l).is_empty(), "{l:?}");
    }
}

#[test]
fn carriage_return_and_uppercase_hex_accepted() {
    let fx = Fixture::new();
    let mut s = fx.session();
    registered(&mut s, &bob(), Limits::default());
    let l = store_line(
        7,
        &o4(&owner()).to_uppercase(),
        &hex(&bob()).to_uppercase(),
        &"AB".repeat(8),
        &[b'x'; 40],
    ) + "\r\n";
    assert_eq!(
        lines(&mut s, &l),
        [format!("mbx.store 7 00 {}", T0 + 7 * DAY)]
    );
}

#[test]
fn fetch_reply_is_padded_base64() {
    let fx = Fixture::new();
    let mut s = fx.session();
    registered(&mut s, &bob(), Limits::default());
    let payload: Vec<u8> = (0..41).collect();
    lines(
        &mut s,
        &store_line(2, &o4(&owner()), &hex(&bob()), &"11".repeat(8), &payload),
    );
    let r = lines(&mut s, &fetch_line(3, &hex(&owner()), 0, "00000001", &[]));
    assert_eq!(r, [format!("mbx.fetch 3 00 0 0 {}", b64(&payload))]);
    assert!(r[0].ends_with('='));
}

#[test]
fn lines_processed_in_order_by_serve() {
    let fx = Fixture::new();
    let mut s = fx.session();
    registered(&mut s, &bob(), Limits::default());
    let (o, b) = (o4(&owner()), hex(&bob()));
    let out = run(
        &mut s,
        vec![
            line(store_line(10, &o, &b, &"22".repeat(8), &[b'a'; 30])),
            line("debug noise"),
            line(store_line(11, &o, &b, &"22".repeat(8), &[b'a'; 30])),
            line(stat_line(12, &b, &[&"22".repeat(8)])),
        ],
    );
    let heads: Vec<Vec<&str>> = out.iter().map(|l| l.split(' ').take(3).collect()).collect();
    assert_eq!(
        heads,
        [
            ["mbx.store", "10", "00"],
            ["mbx.store", "11", "01"],
            ["mbx.stat", "12", "00"]
        ]
    );
}

// ---- durability ----------------------------------------------------------------------------------------

#[test]
fn synchronous_full() {
    let fx = Fixture::new();
    let s = fx.session();
    let sync: i64 = s
        .mailbox()
        .storage()
        .connection()
        .query_row("PRAGMA synchronous", [], |r| r.get(0))
        .unwrap();
    assert_eq!(sync, 2);
}

const CRASH_CHILD_ENV: &str = "MBXD_TEST_CRASH_DB";

/// Runs only as the child of `crash_between_insert_and_commit_gives_no_reply_and_no_row`: the process dies
/// (abort, no unwinding, no cleanup) exactly when SQLite commits the STORE.
#[test]
fn crash_child() {
    let Ok(path) = std::env::var(CRASH_CHILD_ENV) else {
        return;
    };
    let clock = meshcore_mailboxd::system::FakeClock::new(T0);
    let mut s = session_at(std::path::Path::new(&path), &clock);
    s.mailbox()
        .storage()
        .connection()
        .commit_hook(Some(|| std::process::abort()))
        .unwrap();
    println!("STORING");
    let out = run(
        &mut s,
        vec![line(store_line(
            5,
            &o4(&owner()),
            &hex(&bob()),
            &"33".repeat(8),
            &[b'p'; 50],
        ))],
    );
    println!("REPLIES {out:?}");
}

#[test]
fn crash_between_insert_and_commit_gives_no_reply_and_no_row() {
    let fx = Fixture::new();
    {
        let mut s = fx.session();
        registered(&mut s, &bob(), Limits::default());
    }
    let child = std::process::Command::new(std::env::current_exe().unwrap())
        .args(["--exact", "crash_child", "--nocapture", "--test-threads=1"])
        .env(CRASH_CHILD_ENV, fx.path())
        .output()
        .unwrap();
    let stdout = String::from_utf8_lossy(&child.stdout);
    #[cfg(unix)]
    {
        use std::os::unix::process::ExitStatusExt;
        assert_eq!(
            child.status.signal(),
            Some(6),
            "the child must die of SIGABRT at COMMIT: {stdout}"
        );
    }
    assert!(stdout.contains("STORING"), "{stdout}");
    assert!(
        !stdout.contains("REPLIES"),
        "no reply before the commit: {stdout}"
    );
    assert!(!stdout.contains("mbx.store"), "{stdout}");

    let mut s = fx.session();
    assert_eq!(
        lines(&mut s, &stat_line(6, &hex(&bob()), &[&"33".repeat(8)])),
        [format!("mbx.stat 6 00 0:{}", zeros(6))]
    );
    let again = store_line(7, &o4(&owner()), &hex(&bob()), &"33".repeat(8), &[b'p'; 50]);
    assert_eq!(
        lines(&mut s, &again),
        [format!("mbx.store 7 00 {}", T0 + 7 * DAY)]
    );
}

#[test]
fn failed_commit_answers_no_storage_and_rolls_back() {
    let fx = Fixture::new();
    let mut s = fx.session();
    registered(&mut s, &bob(), Limits::default());
    let conn = s.mailbox().storage().connection();
    conn.commit_hook(Some(|| true)).unwrap();
    assert_eq!(
        lines(
            &mut s,
            &store_line(5, &o4(&owner()), &hex(&bob()), &"44".repeat(8), &[b'p'; 50])
        ),
        ["mbx.store 5 13 0"]
    );
    assert_eq!(
        lines(&mut s, &fetch_line(6, &hex(&owner()), 0, "00000001", &[])),
        ["mbx.fetch 6 13 0 0 -"]
    );
    assert_eq!(
        lines(
            &mut s,
            &reg_line(
                7,
                &hex(&carol()),
                &o4(&owner()),
                &hex(&token(&carol(), &k_owner()))
            )
        ),
        ["mbx.reg 7 13 0 0"]
    );
    assert_eq!(
        lines(&mut s, &stat_line(8, &hex(&bob()), &[&"44".repeat(8)])),
        ["mbx.stat 8 13 -"]
    );
    s.mailbox()
        .storage()
        .connection()
        .commit_hook(None::<fn() -> bool>)
        .unwrap();
    assert_eq!(
        lines(&mut s, &stat_line(9, &hex(&bob()), &[&"44".repeat(8)])),
        [format!("mbx.stat 9 00 0:{}", zeros(6))]
    );
    let sent = lines(&mut s, &fetch_line(10, &hex(&owner()), 0, "00000001", &[]));
    assert_eq!(
        sent,
        ["mbx.fetch 10 00 0 0 -"],
        "the failed FETCH recorded no store_id and handed nothing out"
    );
}

#[test]
fn restart_keeps_state_and_store_id() {
    let fx = Fixture::new();
    let (o, b) = (o4(&owner()), hex(&bob()));
    let ack = format!("{}0000", "aa".repeat(4));
    {
        let mut s = fx.session();
        registered(&mut s, &bob(), Limits::default());
        lines(
            &mut s,
            &store_line(2, &o, &b, &"01".repeat(8), &b"one".repeat(10)),
        );
        lines(
            &mut s,
            &store_line(3, &o, &b, &"02".repeat(8), &b"two".repeat(10)),
        );
        assert!(
            lines(&mut s, &fetch_line(4, &hex(&owner()), 0, "0000abcd", &[]))[0]
                .starts_with("mbx.fetch 4 00 1 0 ")
        );
        lines(
            &mut s,
            &fetch_line(
                5,
                &hex(&owner()),
                2,
                "0000abcd",
                &[(&"01".repeat(8), 0, &ack)],
            ),
        );
    }
    fx.clock.set(T0 + 60);
    let mut s = fx.session();
    assert_eq!(
        lines(
            &mut s,
            &stat_line(6, &b, &[&"01".repeat(8), &"02".repeat(8)])
        ),
        [format!("mbx.stat 6 00 2:{ack},1:{}", zeros(6))]
    );
    assert_eq!(
        lines(
            &mut s,
            &reg_line(7, &b, &o, &hex(&token(&bob(), &k_owner())))
        ),
        ["mbx.reg 7 00 7 20"]
    );
    // same store_id after the restart: no resync, so only the second message is offered
    assert_eq!(
        lines(&mut s, &fetch_line(8, &hex(&owner()), 0, "0000abcd", &[])),
        [format!("mbx.fetch 8 00 0 0 {}", b64(&b"two".repeat(10)))]
    );
    assert_eq!(
        lines(&mut s, &fetch_line(9, &hex(&owner()), 2, "0000ffff", &[])),
        ["mbx.fetch 9 00 2 0 -"],
        "new store_id resyncs"
    );
}

#[test]
fn ciphertext_wiped_when_delivered() {
    let fx = Fixture::new();
    let mut s = fx.session();
    registered(&mut s, &bob(), Limits::default());
    lines(
        &mut s,
        &store_line(2, &o4(&owner()), &hex(&bob()), &"05".repeat(8), &[b's'; 30]),
    );
    lines(&mut s, &fetch_line(3, &hex(&owner()), 0, "00000001", &[]));
    lines(
        &mut s,
        &fetch_line(
            4,
            &hex(&owner()),
            0,
            "00000001",
            &[(&"05".repeat(8), 1, &"bb".repeat(6))],
        ),
    );
    let rows: Vec<(Option<Vec<u8>>, String)> = s
        .mailbox()
        .storage()
        .connection()
        .prepare("SELECT payload, state FROM messages")
        .unwrap()
        .query_map([], |r| Ok((r.get(0)?, r.get(1)?)))
        .unwrap()
        .collect::<Result<_, _>>()
        .unwrap();
    assert_eq!(rows, [(None, State::Delivered.db_name().to_string())]);
}

// ---- admin ---------------------------------------------------------------------------------------------

#[test]
fn owner_add_generates_key_and_rejects_prefix_collision() {
    let fx = Fixture::new();
    let mut s = fx.session();
    let pk = Pubkey(owner());
    let k = s
        .mailbox_mut()
        .owner_add(T0, &pk, None, Limits::default())
        .unwrap();
    assert_ne!(k, KOwner([0; 16]));
    let k2 = s
        .mailbox_mut()
        .owner_add(T0, &pk, None, Limits::new(3, 30, 20).unwrap())
        .unwrap();
    assert_eq!(k2, k, "updating limits keeps the key");
    let mut other = owner();
    other[4..].fill(0);
    let err = s
        .mailbox_mut()
        .owner_add(T0, &Pubkey(other), None, Limits::default())
        .unwrap_err();
    assert!(
        matches!(err, AdminError::PrefixTaken(p) if p == pk.owner4()),
        "{err}"
    );
    let k3 = s
        .mailbox_mut()
        .owner_add(T0, &pk, Some(KOwner([5; 16])), Limits::default())
        .unwrap();
    assert_eq!(k3, KOwner([5; 16]), "an explicit key replaces the old one");
    let other_owner = Pubkey(sha256(b"owner2"));
    let k4 = s
        .mailbox_mut()
        .owner_add(T0, &other_owner, None, Limits::default())
        .unwrap();
    assert_ne!(k4, k, "keys are random");
}

#[test]
fn deny_removes_registration_and_undeny_allows_again() {
    let fx = Fixture::new();
    let mut s = fx.session();
    registered(&mut s, &bob(), Limits::default());
    let (o, b, tok) = (o4(&owner()), hex(&bob()), hex(&token(&bob(), &k_owner())));
    let owner4 = Pubkey(owner()).owner4();
    s.mailbox_mut().deny(T0, &owner4, &Pubkey(bob())).unwrap();
    assert_eq!(
        lines(&mut s, &reg_line(2, &b, &o, &tok)),
        ["mbx.reg 2 10 0 0"]
    );
    s.mailbox_mut().undeny(&owner4, &Pubkey(bob())).unwrap();
    assert_eq!(
        lines(&mut s, &store_line(3, &o, &b, &"06".repeat(8), &[b'x'; 30])),
        ["mbx.store 3 10 0"],
        "deny dropped the registration"
    );
    assert_eq!(
        lines(&mut s, &reg_line(4, &b, &o, &tok)),
        ["mbx.reg 4 00 7 20"]
    );
    let err = s
        .mailbox_mut()
        .deny(T0, &Owner4([0xff; 4]), &Pubkey(bob()))
        .unwrap_err();
    assert!(matches!(err, AdminError::UnknownOwner(_)), "{err}");
}

#[test]
fn hello_announces_owners_first_then_recent_depositors() {
    let fx = Fixture::new();
    let mut s = fx.session();
    let owner2 = sha256(b"owner2");
    let mut t = T0;
    add_owner(&mut s, t, &owner(), &k_owner(), Limits::default());
    t += 1;
    add_owner(&mut s, t, &owner2, &k_owner(), Limits::default());
    let senders: Vec<[u8; 32]> = (0..70)
        .map(|i| sha256(format!("d{i}").as_bytes()))
        .collect();
    for (i, d) in senders.iter().enumerate() {
        t += 1;
        fx.clock.set(t);
        lines(
            &mut s,
            &reg_line(
                u32::try_from(i).unwrap(),
                &hex(d),
                &o4(&owner()),
                &hex(&token(d, &k_owner())),
            ),
        );
    }
    t += 1;
    fx.clock.set(t);
    lines(
        &mut s,
        &reg_line(
            99,
            &hex(&senders[0]),
            &o4(&owner2),
            &hex(&token(&senders[0], &k_owner())),
        ),
    );
    s.mailbox_mut()
        .deny(t, &Pubkey(owner()).owner4(), &Pubkey(senders[69]))
        .unwrap();

    let out = lines(&mut s, "@MBX HELLO 2 1.0-rdm");
    assert_eq!(
        out[out.len() - 2..],
        ["mbx.ready 2".to_string(), format!("mbx.time {t}")]
    );
    let acl: Vec<Vec<&str>> = out[..out.len() - 2]
        .iter()
        .map(|l| l.split(' ').collect())
        .collect();
    assert_eq!(acl.len(), 64);
    assert_eq!(acl[0], ["mbx.acl", &hex(&owner()), "o", &o4(&owner())]);
    assert_eq!(acl[1], ["mbx.acl", &hex(&owner2), "o", &o4(&owner2)]);
    assert_eq!(acl[2], ["mbx.acl", &hex(&senders[0]), "d", &o4(&owner2)]);
    assert_eq!(
        acl[3],
        ["mbx.acl", &hex(&senders[68]), "d", &o4(&owner())],
        "denied depositor left out"
    );
    let pubs: std::collections::HashSet<&str> = acl.iter().map(|a| a[1]).collect();
    assert_eq!(pubs.len(), 64, "one line per pubkey");
}

#[test]
fn hello_without_owners_is_ready_and_time() {
    let fx = Fixture::new();
    let mut s = fx.session();
    assert_eq!(
        lines(&mut s, "@MBX HELLO 2"),
        ["mbx.ready 2".to_string(), format!("mbx.time {T0}")]
    );
}

// ---- session protocol v2 -------------------------------------------------------------------------------

#[test]
fn serve_asks_for_hello_at_start_and_a_hello_may_come_any_time() {
    let fx = Fixture::new();
    let mut s = fx.session();
    add_owner(&mut s, T0, &owner(), &k_owner(), Limits::default());
    let acl = format!("mbx.acl {} o {}", hex(&owner()), o4(&owner()));
    let series = |t: i64| vec![acl.clone(), "mbx.ready 2".into(), format!("mbx.time {t}")];
    let mut out = Vec::new();
    serve(&mut s, std::iter::empty(), &mut out).unwrap();
    assert_eq!(String::from_utf8(out).unwrap(), "mbx.hello?\n");
    assert_eq!(
        run(&mut s, vec![line("@MBX HELLO 2 1.0-rdm")]),
        series(T0),
        "the radio booted into a running daemon"
    );
    assert_eq!(
        run(&mut s, vec![line("@MBX HELLO 2 1.0-rdm")]),
        series(T0),
        "a HELLO while ready (the answer to a second mbx.hello?) gets the series again"
    );
}

#[test]
fn hello_with_another_or_no_protocol_gets_only_ready() {
    let fx = Fixture::new();
    let mut s = fx.session();
    add_owner(&mut s, T0, &owner(), &k_owner(), Limits::default());
    for hello in [
        "@MBX HELLO",
        "@MBX HELLO 1.0-rdm",
        "@MBX HELLO sim",
        "@MBX HELLO 1 1.0-rdm",
        "@MBX HELLO 3 1.0-rdm",
        "@MBX HELLO 3",
    ] {
        assert_eq!(lines(&mut s, hello), ["mbx.ready 2"], "{hello}");
    }
    fx.clock.set(T0 + 3600);
    assert!(
        run(&mut s, vec![None]).is_empty(),
        "a mismatch does not start the mbx.time beacon"
    );
    assert_eq!(
        lines(&mut s, "@MBX HELLO 2 1.0-rdm"),
        [
            format!("mbx.acl {} o {}", hex(&owner()), o4(&owner())),
            "mbx.ready 2".into(),
            format!("mbx.time {}", T0 + 3600)
        ],
        "a later HELLO 2 gets the full series"
    );
}

#[test]
fn acl_change_by_another_process_is_pushed_without_restart() {
    let fx = Fixture::new();
    let mut s = fx.session();
    let mut admin = Mailbox::new(SqliteStorage::open(fx.path()).unwrap());
    let owner2 = sha256(b"owner2");
    let acl_owner = format!("mbx.acl {} o {}", hex(&owner()), o4(&owner()));
    let acl_owner2 = format!("mbx.acl {} o {}", hex(&owner2), o4(&owner2));
    let owner4 = Pubkey(owner()).owner4();
    admin
        .owner_add(
            T0,
            &Pubkey(owner()),
            Some(KOwner(k_owner())),
            Limits::default(),
        )
        .unwrap();
    assert_eq!(
        run(&mut s, vec![line("@MBX HELLO 2 1.0-rdm")]),
        [
            acl_owner.clone(),
            "mbx.ready 2".into(),
            format!("mbx.time {T0}")
        ]
    );
    assert!(run(&mut s, vec![None]).is_empty(), "nothing changed");

    admin
        .owner_add(
            T0 + 1,
            &Pubkey(owner2),
            Some(KOwner(k_owner())),
            Limits::default(),
        )
        .unwrap();
    assert_eq!(
        run(&mut s, vec![None]),
        [acl_owner.clone(), acl_owner2.clone(), "mbx.ready 2".into()],
        "owner-add in another process: the series again, without mbx.time"
    );
    let reg = reg_line(
        1,
        &hex(&bob()),
        &o4(&owner()),
        &hex(&token(&bob(), &k_owner())),
    );
    assert_eq!(
        run(&mut s, vec![line(reg), None]),
        ["mbx.reg 1 00 7 20"],
        "the session's own registration is no push"
    );
    admin.deny(T0 + 2, &owner4, &Pubkey(bob())).unwrap();
    assert_eq!(
        run(&mut s, vec![None]),
        [acl_owner.clone(), acl_owner2.clone(), "mbx.ready 2".into()],
        "deny: the depositor is gone from the series"
    );
    admin.undeny(&owner4, &Pubkey(bob())).unwrap();
    assert_eq!(
        run(&mut s, vec![None]),
        [acl_owner.clone(), acl_owner2.clone(), "mbx.ready 2".into()],
        "undeny: pushed again, the depositor has to register first"
    );
    assert!(run(&mut s, vec![None]).is_empty(), "reported once");
}

#[test]
fn requests_are_answered_whatever_the_session_state() {
    let fx = Fixture::new();
    let mut s = fx.session();
    add_owner(&mut s, T0, &owner(), &k_owner(), Limits::default());
    let reg = |rid| {
        reg_line(
            rid,
            &hex(&bob()),
            &o4(&owner()),
            &hex(&token(&bob(), &k_owner())),
        )
    };
    assert_eq!(
        lines(&mut s, &reg(1)),
        ["mbx.reg 1 00 7 20"],
        "before any HELLO"
    );
    assert_eq!(lines(&mut s, "@MBX HELLO 1.0-rdm"), ["mbx.ready 2"]);
    assert_eq!(
        lines(&mut s, &reg(2)),
        ["mbx.reg 2 00 7 20"],
        "after a mismatch"
    );
    let store = store_line(3, &o4(&owner()), &hex(&bob()), &"01".repeat(8), &[b'x'; 10]);
    assert_eq!(
        lines(&mut s, &store),
        [format!("mbx.store 3 00 {}", T0 + 7 * DAY)]
    );
}

#[test]
fn time_every_600_s_after_hello_only() {
    let fx = Fixture::new();
    let mut s = fx.session();
    let stat = |rid| line(stat_line(rid, &hex(&bob()), &[&"01".repeat(8)]));
    let unknown = |rid| format!("mbx.stat {rid} 00 0:{}", zeros(6));
    assert!(run(&mut s, vec![None]).is_empty());
    fx.clock.set(T0 + 3600);
    assert_eq!(
        run(&mut s, vec![None, stat(1)]),
        [unknown(1)],
        "no mbx.time before HELLO"
    );

    let hello_t = T0 + 3600;
    assert_eq!(
        run(&mut s, vec![line("@MBX HELLO 2")]),
        ["mbx.ready 2".to_string(), format!("mbx.time {hello_t}")]
    );
    fx.clock.set(hello_t + 599);
    assert_eq!(run(&mut s, vec![None, stat(2)]), [unknown(2)]);
    fx.clock.set(hello_t + 600);
    assert_eq!(
        run(&mut s, vec![stat(3), None]),
        [unknown(3), format!("mbx.time {}", hello_t + 600)]
    );
    fx.clock.set(hello_t + 1199);
    assert!(run(&mut s, vec![None]).is_empty());
    fx.clock.set(hello_t + 1300);
    assert_eq!(
        run(&mut s, vec![None]),
        [format!("mbx.time {}", hello_t + 1300)]
    );
}

#[test]
fn time_line_sets_a_fake_clock_and_a_due_time_follows() {
    let fx = Fixture::new();
    let mut s = fx.session();
    assert_eq!(
        run(&mut s, vec![line("@MBX HELLO 2")]),
        ["mbx.ready 2".to_string(), format!("mbx.time {T0}")]
    );
    assert!(run(&mut s, vec![line(format!("@MBX TIME {}", T0 + 599))]).is_empty());
    assert_eq!(fx.clock.now(), T0 + 599);
    assert_eq!(
        run(&mut s, vec![line(format!("@MBX TIME {}", T0 + 600))]),
        [format!("mbx.time {}", T0 + 600)]
    );
}

#[test]
fn rotation_survives_restart() {
    let fx = Fixture::new();
    let (first, second) = if bob() < carol() {
        (bob(), carol())
    } else {
        (carol(), bob())
    };
    let o = o4(&owner());
    {
        let mut s = fx.session();
        registered(&mut s, &bob(), Limits::default());
        lines(
            &mut s,
            &reg_line(2, &hex(&carol()), &o, &hex(&token(&carol(), &k_owner()))),
        );
        lines(
            &mut s,
            &store_line(3, &o, &hex(&first), &"01".repeat(8), &b"first".repeat(4)),
        );
        lines(
            &mut s,
            &store_line(4, &o, &hex(&first), &"02".repeat(8), b"first-again"),
        );
        lines(
            &mut s,
            &store_line(5, &o, &hex(&second), &"03".repeat(8), &b"second".repeat(4)),
        );
        assert!(
            lines(&mut s, &fetch_line(6, &hex(&owner()), 0, "00000001", &[]))[0]
                .ends_with(&b64(&b"first".repeat(4)))
        );
    }
    let mut s = fx.session();
    let r = lines(
        &mut s,
        &fetch_line(
            7,
            &hex(&owner()),
            0,
            "00000001",
            &[(&"01".repeat(8), 0, &"11".repeat(6))],
        ),
    );
    assert_eq!(
        r,
        [format!("mbx.fetch 7 00 1 1 {}", b64(&b"second".repeat(4)))]
    );
}

#[test]
fn opens_database_without_rotation_column() {
    let fx = Fixture::new();
    rusqlite::Connection::open(fx.path())
        .unwrap()
        .execute_batch(&meshcore_mailboxd::storage::SCHEMA.replace(", last_sender BLOB", ""))
        .unwrap();
    let mut s = fx.session();
    registered(&mut s, &bob(), Limits::default());
    lines(
        &mut s,
        &store_line(2, &o4(&owner()), &hex(&bob()), &"01".repeat(8), &[b'x'; 20]),
    );
    assert!(
        lines(&mut s, &fetch_line(3, &hex(&owner()), 0, "00000001", &[]))[0]
            .starts_with("mbx.fetch 3 00 0 0 ")
    );
}
