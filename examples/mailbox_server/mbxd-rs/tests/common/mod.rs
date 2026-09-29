//! Shared by the integration tests. Request lines are built here from the format in `06` par. 3.16, not with
//! the crate's own formatter, so a formatting bug cannot hide a parsing one.
#![allow(dead_code)]

use std::path::{Path, PathBuf};

use base64::Engine as _;
use hmac::{KeyInit, Mac};
use mbxd::domain::model::Limits;
use mbxd::mailbox::Mailbox;
use mbxd::session::Session;
use mbxd::storage::SqliteStorage;
pub use mbxd::system::Clock;
use mbxd::system::FakeClock;
use mbxd::types::{KOwner, Pubkey, UnixTime};
use sha2::{Digest, Sha256};

pub const T0: UnixTime = 1_790_000_000;
pub const DAY: UnixTime = 86_400;

pub fn sha256(data: &[u8]) -> [u8; 32] {
    Sha256::digest(data).into()
}

pub fn owner() -> [u8; 32] {
    sha256(b"owner")
}

pub fn k_owner() -> [u8; 16] {
    sha256(b"k")[..16].try_into().unwrap()
}

pub fn bob() -> [u8; 32] {
    sha256(b"bob")
}

pub fn carol() -> [u8; 32] {
    sha256(b"carol")
}

pub fn hex(b: &[u8]) -> String {
    b.iter().map(|x| format!("{x:02x}")).collect()
}

pub fn unhex(s: &str) -> Vec<u8> {
    (0..s.len())
        .step_by(2)
        .map(|i| u8::from_str_radix(&s[i..i + 2], 16).unwrap())
        .collect()
}

pub fn b64(b: &[u8]) -> String {
    base64::engine::general_purpose::STANDARD.encode(b)
}

pub fn unb64(s: &str) -> Vec<u8> {
    base64::engine::general_purpose::STANDARD.decode(s).unwrap()
}

pub fn token(sender: &[u8], k: &[u8]) -> [u8; 8] {
    let mut mac = hmac::Hmac::<Sha256>::new_from_slice(k).unwrap();
    mac.update(sender);
    mac.finalize().into_bytes()[..8].try_into().unwrap()
}

pub fn o4(pubkey: &[u8]) -> String {
    hex(&pubkey[..4])
}

pub fn store_line(rid: u32, owner4: &str, sender: &str, pkt_hash: &str, payload: &[u8]) -> String {
    format!(
        "@MBX STORE {rid} {owner4} {sender} {pkt_hash} {}",
        b64(payload)
    )
}

pub fn reg_line(rid: u32, sender: &str, owner4: &str, tok: &str) -> String {
    format!("@MBX REG {rid} {sender} {owner4} {tok}")
}

/// Reports as (hash, result, ack) hex strings.
pub fn fetch_line(
    rid: u32,
    client: &str,
    flags: u8,
    store_id: &str,
    reports: &[(&str, u8, &str)],
) -> String {
    let rep: Vec<String> = reports
        .iter()
        .map(|(h, r, a)| format!("{h}:{r}:{a}"))
        .collect();
    let rep = if rep.is_empty() {
        "-".to_string()
    } else {
        rep.join(",")
    };
    format!("@MBX FETCH {rid} {client} {flags:02x} {store_id} {rep}")
}

pub fn stat_line(rid: u32, sender: &str, hashes: &[&str]) -> String {
    format!("@MBX STAT {rid} {sender} {}", hashes.join(","))
}

pub type TestSession = Session<SqliteStorage, FakeClock>;

pub fn session_at(path: &Path, clock: &FakeClock) -> TestSession {
    Session::new(
        Mailbox::new(SqliteStorage::open(path).unwrap()),
        clock.clone(),
    )
}

pub struct Fixture {
    pub dir: tempfile::TempDir,
    pub clock: FakeClock,
}

impl Fixture {
    pub fn new() -> Self {
        Fixture {
            dir: tempfile::tempdir().unwrap(),
            clock: FakeClock::new(T0),
        }
    }

    pub fn path(&self) -> PathBuf {
        self.dir.path().join("mbxd.db")
    }

    pub fn session(&self) -> TestSession {
        session_at(&self.path(), &self.clock)
    }
}

pub fn add_owner(
    s: &mut TestSession,
    now: UnixTime,
    pubkey: &[u8],
    k: &[u8],
    limits: Limits,
) -> KOwner {
    let k = KOwner(k.try_into().unwrap());
    s.mailbox_mut()
        .owner_add(now, &Pubkey(pubkey.try_into().unwrap()), Some(k), limits)
        .unwrap()
}

/// OWNER added with K_OWNER and `sender` registered, as `registered()` in test_mbxd.py.
pub fn registered(s: &mut TestSession, sender: &[u8], limits: Limits) {
    add_owner(s, T0, &owner(), &k_owner(), limits);
    let r = s
        .handle_line(&reg_line(
            1,
            &hex(sender),
            &o4(&owner()),
            &hex(&token(sender, &k_owner())),
        ))
        .unwrap();
    assert!(
        r.len() == 1 && r[0].to_string().starts_with("mbx.reg 1 00 "),
        "{r:?}"
    );
}

pub fn lines(s: &mut TestSession, line: &str) -> Vec<String> {
    s.handle_line(line)
        .unwrap()
        .iter()
        .map(ToString::to_string)
        .collect()
}

/// The `mbxd` binary built for these tests.
pub fn mbxd_bin() -> PathBuf {
    PathBuf::from(env!("CARGO_BIN_EXE_mbxd"))
}

/// `examples/mailbox_server/mbxd/mbxd.py`, the reference implementation.
pub fn mbxd_py() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../mbxd/mbxd.py")
}

pub fn python3_available() -> bool {
    std::process::Command::new("python3")
        .arg("--version")
        .output()
        .is_ok_and(|o| o.status.success())
}
