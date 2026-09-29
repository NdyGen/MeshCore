//! Every case of `test/rdm_vectors/mailbox_conformance.json` (06 par. 3.17), through the line protocol like
//! `test_mbxd.py`: request lines in, reply lines out.

mod common;

use common::*;
use mbxd::domain::model::Limits;
use mbxd::domain::policy;
use mbxd::types::{KOwner, Owner4, Pubkey};
use serde_json::Value;

fn vectors() -> Value {
    let path = std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
        .join("../../../test/rdm_vectors/mailbox_conformance.json");
    serde_json::from_str(&std::fs::read_to_string(&path).unwrap()).unwrap()
}

fn s<'a>(v: &'a Value, k: &str) -> &'a str {
    v[k].as_str().unwrap_or_else(|| panic!("{k} in {v}"))
}

fn n(v: &Value, k: &str) -> i64 {
    v[k].as_i64().unwrap_or_else(|| panic!("{k} in {v}"))
}

/// The single reply's fields after `mbx.<kind> <rid>`, with the code checked to be two lowercase hex digits.
fn one_reply(replies: Vec<String>, kind: &str, rid: u32) -> Vec<String> {
    assert_eq!(replies.len(), 1, "{replies:?}");
    let parts: Vec<String> = replies[0].split(' ').map(String::from).collect();
    assert_eq!(
        (parts[0].as_str(), parts[1].as_str()),
        (format!("mbx.{kind}").as_str(), rid.to_string().as_str()),
        "{}",
        replies[0]
    );
    assert!(
        parts[2].len() == 2 && parts[2] == parts[2].to_lowercase(),
        "code is two lowercase hex digits"
    );
    parts[2..].to_vec()
}

fn code(s: &str) -> i64 {
    i64::from_str_radix(s, 16).unwrap()
}

fn run_step(session: &mut TestSession, fx: &Fixture, rid: u32, step: &Value) {
    let t = n(step, "t");
    fx.clock.set(t);
    let (a, exp) = (&step["args"], &step["expect"]);
    match s(step, "op") {
        "owner_add" => {
            let limits = Limits::new(n(a, "ttl_days"), n(a, "sync_days"), n(a, "quota")).unwrap();
            add_owner(
                session,
                t,
                &unhex(s(a, "pub")),
                &unhex(s(a, "k_owner")),
                limits,
            );
        }
        "deny" => {
            let owner4 = Owner4(unhex(s(a, "owner")).try_into().unwrap());
            session
                .mailbox_mut()
                .deny(t, &owner4, &Pubkey(unhex(s(a, "pub")).try_into().unwrap()))
                .unwrap();
        }
        "advance" => session.mailbox_mut().advance(t).unwrap(),
        "reg" => {
            let r = lines(
                session,
                &reg_line(rid, s(a, "sender"), s(a, "owner"), s(a, "token")),
            );
            let f = one_reply(r, "reg", rid);
            let got = (
                code(&f[0]),
                f[1].parse::<i64>().unwrap(),
                f[2].parse::<i64>().unwrap(),
            );
            assert_eq!(got, (n(exp, "code"), n(exp, "ttl_days"), n(exp, "quota")));
        }
        "store" => {
            let line = store_line(
                rid,
                s(a, "owner"),
                s(a, "sender"),
                s(a, "hash"),
                &unhex(s(a, "payload")),
            );
            let f = one_reply(lines(session, &line), "store", rid);
            assert_eq!(
                (code(&f[0]), f[1].parse::<i64>().unwrap()),
                (n(exp, "code"), n(exp, "expires"))
            );
        }
        "fetch" => {
            let reports: Vec<(&str, u8, &str)> = a["reports"]
                .as_array()
                .unwrap()
                .iter()
                .map(|r| {
                    (
                        s(r, "hash"),
                        u8::try_from(n(r, "result")).unwrap(),
                        s(r, "ack"),
                    )
                })
                .collect();
            let flags = u8::try_from(n(a, "flags")).unwrap();
            let line = fetch_line(rid, s(a, "client"), flags, s(a, "store_id"), &reports);
            let f = one_reply(lines(session, &line), "fetch", rid);
            let payload = (f[3] != "-").then(|| hex(&unb64(&f[3])));
            let got = (
                code(&f[0]),
                f[1].parse::<i64>().unwrap(),
                f[2].parse::<i64>().unwrap(),
                payload,
            );
            let want = (
                n(exp, "code"),
                n(exp, "remaining"),
                n(exp, "reports_ok"),
                exp["payload"].as_str().map(String::from),
            );
            assert_eq!(got, want);
        }
        "stat" => {
            let hashes: Vec<&str> = a["hashes"]
                .as_array()
                .unwrap()
                .iter()
                .map(|h| h.as_str().unwrap())
                .collect();
            let f = one_reply(
                lines(session, &stat_line(rid, s(a, "sender"), &hashes)),
                "stat",
                rid,
            );
            assert_eq!(code(&f[0]), n(exp, "code"));
            let got: Vec<(i64, String)> = if f[1] == "-" {
                vec![]
            } else {
                f[1].split(',')
                    .map(|i| i.split_once(':').unwrap())
                    .map(|(st, ack)| (st.parse().unwrap(), ack.to_string()))
                    .collect()
            };
            let want: Vec<(i64, String)> = exp["items"]
                .as_array()
                .unwrap()
                .iter()
                .map(|i| (n(i, "state"), s(i, "ack").to_string()))
                .collect();
            assert_eq!(got, want);
        }
        op => panic!("unknown op {op}"),
    }
}

#[test]
fn every_conformance_case() {
    let vectors = vectors();
    let cases = vectors["cases"].as_array().unwrap();
    assert!(cases.len() >= 39, "{} cases", cases.len());
    let mut failures = Vec::new();
    for case in cases {
        let name = s(case, "name");
        let fx = Fixture::new();
        let mut session = fx.session();
        for (rid, step) in case["steps"].as_array().unwrap().iter().enumerate() {
            let rid = u32::try_from(rid).unwrap();
            let r = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
                run_step(&mut session, &fx, rid, step)
            }));
            if let Err(e) = r {
                let msg = e
                    .downcast_ref::<String>()
                    .cloned()
                    .or_else(|| e.downcast_ref::<&str>().map(|s| s.to_string()));
                failures.push(format!(
                    "{name} step {rid} ({} at t={}): {}",
                    s(step, "op"),
                    n(step, "t"),
                    msg.unwrap_or_default()
                ));
                break;
            }
        }
    }
    assert!(
        failures.is_empty(),
        "{} of {} cases failed:\n{}",
        failures.len(),
        cases.len(),
        failures.join("\n")
    );
}

#[test]
fn valid_tokens_in_the_vectors_match_our_hmac() {
    let vectors = vectors();
    let mut owners = std::collections::HashMap::new();
    let mut checked = 0;
    for case in vectors["cases"].as_array().unwrap() {
        for step in case["steps"].as_array().unwrap() {
            let a = &step["args"];
            match s(step, "op") {
                "owner_add" => {
                    owners.insert(
                        s(a, "pub")[..8].to_string(),
                        KOwner(unhex(s(a, "k_owner")).try_into().unwrap()),
                    );
                }
                "reg" if n(&step["expect"], "code") == 0 => {
                    let sender = Pubkey(unhex(s(a, "sender")).try_into().unwrap());
                    assert_eq!(
                        hex(&policy::token(&owners[s(a, "owner")], &sender).0),
                        s(a, "token")
                    );
                    checked += 1;
                }
                _ => {}
            }
        }
    }
    assert!(checked > 10);
}
