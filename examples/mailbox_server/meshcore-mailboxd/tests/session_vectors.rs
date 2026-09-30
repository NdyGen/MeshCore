//! The daemon side of `test/rdm_vectors/mbxd_session.json` (06 par. 3.17): every case whose `players` include
//! `pi`, played against the binary on stdin/stdout with the fake clock. `owner_add`, `deny` and `undeny` run the
//! command line in a second process while `serve` is running, as they do on the Pi. The radio's lines are fed
//! in, and after each `pi` step the daemon must have written exactly the listed lines since the previous one.

mod common;

use std::io::{BufRead, BufReader, Write};
use std::path::{Path, PathBuf};
use std::process::{Child, ChildStdin, Command, Stdio};
use std::sync::mpsc::{self, Receiver, RecvTimeoutError};
use std::time::{Duration, Instant};

use common::*;
use serde_json::Value;

/// A push after an admin command arrives within the daemon's poll interval; a loaded machine gets some slack.
const REPLY_TIMEOUT: Duration = Duration::from_secs(10);
/// Longer than the daemon's poll interval, so a spurious push at the end of a case is still caught.
const QUIET: Duration = Duration::from_millis(1500);

fn vectors() -> Value {
    let path =
        Path::new(env!("CARGO_MANIFEST_DIR")).join("../../../test/rdm_vectors/mbxd_session.json");
    serde_json::from_str(&std::fs::read_to_string(&path).unwrap()).unwrap()
}

fn s<'a>(v: &'a Value, k: &str) -> &'a str {
    v[k].as_str().unwrap_or_else(|| panic!("{k} in {v}"))
}

fn n(v: &Value, k: &str) -> i64 {
    v[k].as_i64().unwrap_or_else(|| panic!("{k} in {v}"))
}

fn strings(v: &Value) -> Vec<String> {
    v.as_array()
        .unwrap()
        .iter()
        .map(|l| l.as_str().unwrap().to_string())
        .collect()
}

/// `serve --stdio --fake-clock` with its stdout read on a thread, so a line the daemon sends on its own
/// (`mbx.hello?`, an ACL push) is seen without a request going in first.
struct Daemon {
    child: Child,
    stdin: Option<ChildStdin>,
    lines: Receiver<String>,
}

impl Daemon {
    fn start(db: &Path) -> Daemon {
        let mut child = Command::new(daemon_bin())
            .arg("--db")
            .arg(db)
            .args(["serve", "--stdio", "--fake-clock"])
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::inherit())
            .spawn()
            .unwrap();
        let stdin = child.stdin.take();
        let stdout = BufReader::new(child.stdout.take().unwrap());
        let (tx, lines) = mpsc::channel();
        std::thread::spawn(move || {
            stdout
                .lines()
                .map_while(Result::ok)
                .for_each(|l| tx.send(l).unwrap_or(()))
        });
        Daemon {
            child,
            stdin,
            lines,
        }
    }

    fn send(&mut self, line: &str) {
        writeln!(self.stdin.as_mut().unwrap(), "{line}").unwrap();
    }

    fn set_clock(&mut self, t: i64) {
        self.send(&format!("@MBX TIME {t}"));
    }

    /// Exactly `want` since the previous `pi` step: every line within the timeout, and nothing else before them.
    fn expect(&self, want: &[String], context: &str) {
        let deadline = Instant::now() + REPLY_TIMEOUT;
        let mut got = Vec::new();
        while got.len() < want.len() {
            match self
                .lines
                .recv_timeout(deadline.saturating_duration_since(Instant::now()))
            {
                Ok(l) => got.push(l),
                Err(RecvTimeoutError::Timeout) => break,
                Err(RecvTimeoutError::Disconnected) => break,
            }
        }
        assert_eq!(got, want, "{context}");
    }

    /// Nothing may arrive for a while (longer than the daemon's poll interval).
    fn expect_quiet(&self, context: &str) {
        match self.lines.recv_timeout(QUIET) {
            Ok(l) => panic!("{context}: unexpected line {l:?}"),
            Err(RecvTimeoutError::Timeout | RecvTimeoutError::Disconnected) => {}
        }
    }

    /// EOF on stdin ends the session; whatever the daemon still wrote must be nothing.
    fn stop(mut self, context: &str) {
        self.expect_quiet(context);
        drop(self.stdin.take());
        let status = self.child.wait().unwrap();
        assert!(status.success(), "{context}: exit {status:?}");
        let rest: Vec<String> = self.lines.try_iter().collect();
        assert!(
            rest.is_empty(),
            "{context}: lines after the last step: {rest:?}"
        );
    }
}

impl Drop for Daemon {
    fn drop(&mut self) {
        let _ = self.child.kill();
        let _ = self.child.wait();
    }
}

fn admin(db: &Path, args: &[&str]) {
    let out = Command::new(daemon_bin())
        .arg("--db")
        .arg(db)
        .args(args)
        .output()
        .unwrap();
    assert!(
        out.status.success(),
        "{args:?}: {}",
        String::from_utf8_lossy(&out.stderr)
    );
}

fn admin_step(db: &Path, op: &str, a: &Value) {
    match op {
        "owner_add" => admin(
            db,
            &[
                "owner-add",
                s(a, "pub"),
                "--k-owner",
                s(a, "k_owner"),
                "--ttl-days",
                &n(a, "ttl_days").to_string(),
                "--sync-days",
                &n(a, "sync_days").to_string(),
                "--quota",
                &n(a, "quota").to_string(),
            ],
        ),
        "deny" | "undeny" => admin(db, &[op, s(a, "owner"), s(a, "pub")]),
        _ => unreachable!(),
    }
}

fn play(name: &str, steps: &[Value], db: PathBuf) {
    let mut daemon: Option<Daemon> = None;
    for (i, step) in steps.iter().enumerate() {
        let context = format!("{name} step {i}: {step}");
        if let Some(from) = step["from"].as_str() {
            let lines = strings(&step["lines"]);
            match (from, daemon.as_mut()) {
                ("radio", Some(d)) => lines.iter().for_each(|l| d.send(l)),
                ("radio", None) => {} // the daemon is off, the radio's lines are lost
                ("pi", Some(d)) => d.expect(&lines, &context),
                ("pi", None) => assert!(lines.is_empty(), "{context}: the daemon is not running"),
                _ => panic!("{context}"),
            }
            continue;
        }
        let op = s(step, "op");
        let t = step["t"].as_i64();
        if op == "pi_start" {
            if let Some(d) = daemon.take() {
                d.stop(&context);
            }
            let mut d = Daemon::start(&db);
            d.set_clock(t.unwrap_or_else(|| panic!("{context}: pi_start without t")));
            daemon = Some(d);
            continue;
        }
        if let (Some(d), Some(t)) = (daemon.as_mut(), t) {
            d.set_clock(t);
        }
        match op {
            "owner_add" | "deny" | "undeny" => admin_step(&db, op, &step["args"]),
            "radio_boot" | "radio_ms" | "radio_reg" | "assert_radio" => {}
            other => panic!("{context}: unknown op {other}"),
        }
    }
    if let Some(d) = daemon {
        d.stop(&format!("{name}: end of case"));
    }
}

#[test]
fn every_session_case_for_the_daemon_side() {
    let vectors = vectors();
    let cases = vectors["cases"].as_array().unwrap();
    let mine: Vec<&Value> = cases
        .iter()
        .filter(|c| {
            c["players"]
                .as_array()
                .is_none_or(|p| p.iter().any(|p| p == "pi"))
        })
        .collect();
    assert_eq!(mine.len(), 7, "cases the daemon side plays");
    let dir = tempfile::tempdir().unwrap();
    let mut failures = Vec::new();
    for case in mine {
        let name = s(case, "name");
        let db = dir.path().join(format!("{name}.db"));
        let steps = case["steps"].as_array().unwrap();
        let r = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| play(name, steps, db)));
        if let Err(e) = r {
            let msg = e
                .downcast_ref::<String>()
                .cloned()
                .or_else(|| e.downcast_ref::<&str>().map(|s| s.to_string()));
            failures.push(msg.unwrap_or_else(|| name.to_string()));
        }
    }
    assert!(
        failures.is_empty(),
        "{} of 7 cases failed:\n{}",
        failures.len(),
        failures.join("\n")
    );
}
