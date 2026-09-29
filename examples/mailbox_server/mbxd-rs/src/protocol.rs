//! The line protocol between the mailbox radio and `mbxd` (`06` par. 3.16), parse and format only.
//!
//! Requests start with `@MBX `; any other line is the radio's debug output. Fields are separated by exactly one
//! space, hex may be upper or lower case, base64 is padded. Replies use lowercase hex and a two-digit code.

use std::fmt;

pub use crate::types::Report;
use crate::types::{Ack, Code, KOwner, Owner4, PktHash, Pubkey, RequestId, Role, Token, UnixTime};

pub const REQUEST_PREFIX: &str = "@MBX ";
/// Most reports in one FETCH and hashes in one STAT.
pub const MAX_BATCH: usize = 8;
pub const FETCH_FLAG_NO_PAYLOAD: u8 = 0x02;

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Request {
    /// The radio booted. The fields after HELLO are free text (its firmware version).
    Hello { firmware: String },
    Store {
        id: RequestId,
        owner4: Owner4,
        sender: Pubkey,
        pkt_hash: PktHash,
        payload: Vec<u8>,
    },
    Reg {
        id: RequestId,
        sender: Pubkey,
        owner4: Owner4,
        token: Token,
    },
    Fetch {
        id: RequestId,
        client: Pubkey,
        flags: u8,
        store_id: u32,
        reports: Vec<Report>,
    },
    Stat {
        id: RequestId,
        sender: Pubkey,
        hashes: Vec<PktHash>,
    },
    /// Simulator only (`serve --stdio --fake-clock`): sets the daemon's clock.
    Time { unix: u32 },
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct StatItem {
    /// [`crate::types::State::wire`], or 0 for UNKNOWN.
    pub state: u8,
    pub ack: Ack,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Reply {
    Acl {
        pubkey: Pubkey,
        role: Role,
        owner4: Owner4,
    },
    Ready,
    Time(UnixTime),
    Store {
        id: RequestId,
        code: Code,
        expires: UnixTime,
    },
    Reg {
        id: RequestId,
        code: Code,
        ttl_days: u8,
        quota: u8,
    },
    Fetch {
        id: RequestId,
        code: Code,
        remaining: u8,
        reports_ok: u8,
        payload: Option<Vec<u8>>,
    },
    Stat {
        id: RequestId,
        code: Code,
        items: Vec<StatItem>,
    },
}

/// Why a line starting with `@MBX ` gets no reply.
#[derive(Debug, Clone, PartialEq, Eq, thiserror::Error)]
pub enum LineError {
    #[error("unknown command {0:?}")]
    UnknownCommand(String),
    #[error("malformed {command}: {error}")]
    Malformed {
        command: &'static str,
        error: FieldError,
    },
}

#[derive(Debug, Clone, PartialEq, Eq, thiserror::Error)]
pub enum FieldError {
    #[error("expected {expected} fields, got {got}")]
    FieldCount { expected: usize, got: usize },
    #[error("bad u32 {0:?}")]
    U32(String),
    #[error("expected {bytes} hex bytes, got {got:?}")]
    Hex { bytes: usize, got: String },
    #[error("empty payload")]
    EmptyPayload,
    #[error("bad base64 {0:?}")]
    Base64(String),
    #[error("bad report result {0:?}")]
    ReportResult(String),
    #[error("more than {MAX_BATCH} {0}")]
    TooMany(&'static str),
}

/// Parses one line from the radio. `Ok(None)`: not a request (debug output, blank line).
pub fn parse_line(line: &str) -> Result<Option<Request>, LineError> {
    let line = line.trim_end_matches(['\r', '\n']);
    let Some(rest) = line.strip_prefix(REQUEST_PREFIX) else {
        return Ok(None);
    };
    let mut parts = rest.split(' ');
    let command = parts.next().unwrap_or_default();
    let fields: Vec<&str> = parts.collect();
    let (command, parsed) = match command {
        "STORE" => ("STORE", parse_store(&fields)),
        "REG" => ("REG", parse_reg(&fields)),
        "FETCH" => ("FETCH", parse_fetch(&fields)),
        "STAT" => ("STAT", parse_stat(&fields)),
        "HELLO" => (
            "HELLO",
            Ok(Request::Hello {
                firmware: fields.join(" "),
            }),
        ),
        // the value may carry surrounding whitespace, as mbxd.py strips it
        "TIME" => (
            "TIME",
            parse_time(rest.strip_prefix("TIME ").unwrap_or_default()),
        ),
        other => return Err(LineError::UnknownCommand(other.to_string())),
    };
    parsed
        .map(Some)
        .map_err(|error| LineError::Malformed { command, error })
}

/// `K_owner` as 32 hex digits, for the command line.
pub fn parse_k_owner(s: &str) -> Result<KOwner, FieldError> {
    hex_array(s).map(KOwner)
}

/// A public key as 64 hex digits, for the command line.
pub fn parse_pubkey(s: &str) -> Result<Pubkey, FieldError> {
    hex_array(s).map(Pubkey)
}

/// An owner as its prefix (8 hex digits) or its public key (64 hex digits), for the command line.
pub fn parse_owner(s: &str) -> Result<Owner4, FieldError> {
    if s.len() == 2 * Pubkey::LEN {
        return parse_pubkey(s).map(|pk| pk.owner4());
    }
    hex_array(s).map(Owner4)
}

fn fields<'a, const N: usize>(f: &[&'a str]) -> Result<[&'a str; N], FieldError> {
    f.try_into().map_err(|_| FieldError::FieldCount {
        expected: N,
        got: f.len(),
    })
}

fn parse_store(f: &[&str]) -> Result<Request, FieldError> {
    let [id, owner4, sender, pkt_hash, payload] = fields(f)?;
    Ok(Request::Store {
        id: u32_field(id)?,
        owner4: Owner4(hex_array(owner4)?),
        sender: Pubkey(hex_array(sender)?),
        pkt_hash: PktHash(hex_array(pkt_hash)?),
        payload: base64_field(payload)?,
    })
}

fn parse_reg(f: &[&str]) -> Result<Request, FieldError> {
    let [id, sender, owner4, token] = fields(f)?;
    Ok(Request::Reg {
        id: u32_field(id)?,
        sender: Pubkey(hex_array(sender)?),
        owner4: Owner4(hex_array(owner4)?),
        token: Token(hex_array(token)?),
    })
}

fn parse_fetch(f: &[&str]) -> Result<Request, FieldError> {
    let [id, client, flags, store_id, reports] = fields(f)?;
    let [flags] = hex_array(flags)?;
    Ok(Request::Fetch {
        id: u32_field(id)?,
        client: Pubkey(hex_array(client)?),
        flags,
        store_id: u32::from_be_bytes(hex_array(store_id)?),
        reports: parse_reports(reports)?,
    })
}

fn parse_reports(s: &str) -> Result<Vec<Report>, FieldError> {
    if s == "-" {
        return Ok(Vec::new());
    }
    let reports = s
        .split(',')
        .map(|item| {
            let [pkt_hash, result, ack] = fields(&item.split(':').collect::<Vec<_>>())?;
            let result = u32_field(result)
                .ok()
                .and_then(|r| u8::try_from(r).ok())
                .ok_or_else(|| FieldError::ReportResult(clip(result)))?;
            Ok(Report {
                pkt_hash: PktHash(hex_array(pkt_hash)?),
                result: result.into(),
                ack: Ack(hex_array(ack)?),
            })
        })
        .collect::<Result<Vec<_>, _>>()?;
    if reports.len() > MAX_BATCH {
        return Err(FieldError::TooMany("reports"));
    }
    Ok(reports)
}

fn parse_stat(f: &[&str]) -> Result<Request, FieldError> {
    let [id, sender, hashes] = fields(f)?;
    let hashes = hashes
        .split(',')
        .map(|h| hex_array(h).map(PktHash))
        .collect::<Result<Vec<_>, _>>()?;
    if hashes.len() > MAX_BATCH {
        return Err(FieldError::TooMany("hashes"));
    }
    Ok(Request::Stat {
        id: u32_field(id)?,
        sender: Pubkey(hex_array(sender)?),
        hashes,
    })
}

fn parse_time(value: &str) -> Result<Request, FieldError> {
    Ok(Request::Time {
        unix: u32_field(value.trim())?,
    })
}

/// 1-10 ASCII digits (leading zeros allowed, no sign), at most `u32::MAX`.
fn u32_field(s: &str) -> Result<u32, FieldError> {
    if s.is_empty() || s.len() > 10 || !s.bytes().all(|b| b.is_ascii_digit()) {
        return Err(FieldError::U32(clip(s)));
    }
    s.parse().map_err(|_| FieldError::U32(clip(s)))
}

fn hex_array<const N: usize>(s: &str) -> Result<[u8; N], FieldError> {
    let bad = || FieldError::Hex {
        bytes: N,
        got: clip(s),
    };
    let digits = s.as_bytes();
    if digits.len() != 2 * N {
        return Err(bad());
    }
    let mut out = [0u8; N];
    for (byte, &[hi, lo]) in out.iter_mut().zip(digits.as_chunks::<2>().0) {
        let nibble = |c: u8| (c as char).to_digit(16).ok_or_else(bad);
        *byte = (nibble(hi)? << 4 | nibble(lo)?) as u8;
    }
    Ok(out)
}

/// Canonical padding and the standard alphabet only, like Python's `b64decode(validate=True)`; unused trailing
/// bits are tolerated there too.
const BASE64: base64::engine::GeneralPurpose = base64::engine::GeneralPurpose::new(
    &base64::alphabet::STANDARD,
    base64::engine::GeneralPurposeConfig::new()
        .with_decode_allow_trailing_bits(true)
        .with_decode_padding_mode(base64::engine::DecodePaddingMode::RequireCanonical),
);

fn base64_field(s: &str) -> Result<Vec<u8>, FieldError> {
    use base64::Engine as _;
    if s.is_empty() {
        return Err(FieldError::EmptyPayload);
    }
    BASE64.decode(s).map_err(|_| FieldError::Base64(clip(s)))
}

fn clip(s: &str) -> String {
    s.chars().take(80).collect()
}

struct Base64OrDash<'a>(Option<&'a [u8]>);

impl fmt::Display for Base64OrDash<'_> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        use base64::Engine as _;
        match self.0 {
            // mbxd.py writes '-' for an empty payload too
            Some(p) if !p.is_empty() => f.write_str(&BASE64.encode(p)),
            _ => f.write_str("-"),
        }
    }
}

/// Comma-separated items, or `-` for none.
fn list<T>(
    f: &mut fmt::Formatter<'_>,
    items: &[T],
    item: impl Fn(&mut fmt::Formatter<'_>, &T) -> fmt::Result,
) -> fmt::Result {
    if items.is_empty() {
        return f.write_str("-");
    }
    for (i, it) in items.iter().enumerate() {
        if i > 0 {
            f.write_str(",")?;
        }
        item(f, it)?;
    }
    Ok(())
}

impl fmt::Display for Reply {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Reply::Acl {
                pubkey,
                role,
                owner4,
            } => {
                let role = match role {
                    Role::Owner => 'o',
                    Role::Depositor => 'd',
                };
                write!(f, "mbx.acl {pubkey} {role} {owner4}")
            }
            Reply::Ready => f.write_str("mbx.ready"),
            Reply::Time(t) => write!(f, "mbx.time {t}"),
            Reply::Store { id, code, expires } => {
                write!(f, "mbx.store {id} {:02x} {expires}", code.byte())
            }
            Reply::Reg {
                id,
                code,
                ttl_days,
                quota,
            } => write!(f, "mbx.reg {id} {:02x} {ttl_days} {quota}", code.byte()),
            Reply::Fetch {
                id,
                code,
                remaining,
                reports_ok,
                payload,
            } => write!(
                f,
                "mbx.fetch {id} {:02x} {remaining} {reports_ok} {}",
                code.byte(),
                Base64OrDash(payload.as_deref())
            ),
            Reply::Stat { id, code, items } => {
                write!(f, "mbx.stat {id} {:02x} ", code.byte())?;
                list(f, items, |f, it| write!(f, "{}:{}", it.state, it.ack))
            }
        }
    }
}

impl fmt::Display for Request {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(REQUEST_PREFIX)?;
        match self {
            Request::Hello { firmware } => write!(f, "HELLO {firmware}"),
            Request::Store {
                id,
                owner4,
                sender,
                pkt_hash,
                payload,
            } => {
                write!(
                    f,
                    "STORE {id} {owner4} {sender} {pkt_hash} {}",
                    Base64OrDash(Some(payload))
                )
            }
            Request::Reg {
                id,
                sender,
                owner4,
                token,
            } => write!(f, "REG {id} {sender} {owner4} {token}"),
            Request::Fetch {
                id,
                client,
                flags,
                store_id,
                reports,
            } => {
                write!(f, "FETCH {id} {client} {flags:02x} {store_id:08x} ")?;
                list(f, reports, |f, r| {
                    write!(f, "{}:{}:{}", r.pkt_hash, u8::from(r.result), r.ack)
                })
            }
            Request::Stat { id, sender, hashes } => {
                write!(f, "STAT {id} {sender} ")?;
                list(f, hashes, |f, h| write!(f, "{h}"))
            }
            Request::Time { unix } => write!(f, "TIME {unix}"),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::types::ReportResult;

    const Z32: &str = "0000000000000000000000000000000000000000000000000000000000000000";
    const Z8: &str = "0000000000000000";

    fn pk(b: u8) -> Pubkey {
        Pubkey([b; 32])
    }

    fn malformed(line: &str) -> bool {
        matches!(
            parse_line(line),
            Err(LineError::Malformed { .. }) | Err(LineError::UnknownCommand(_))
        )
    }

    #[test]
    fn debug_output_and_blank_lines_are_not_requests() {
        for line in [
            "",
            "MBX STORE 1",
            "mbx.store 1 00 5",
            "12:00:00 - 27/9/2026 U RAW: 0102",
            "@MBXSTORE 1",
            "@MBX",
        ] {
            assert_eq!(parse_line(line), Ok(None), "{line:?}");
        }
    }

    #[test]
    fn unknown_commands_are_reported_as_such() {
        assert_eq!(
            parse_line("@MBX NOPE 1"),
            Err(LineError::UnknownCommand("NOPE".into()))
        );
        assert_eq!(
            parse_line("@MBX "),
            Err(LineError::UnknownCommand(String::new()))
        );
        assert_eq!(
            parse_line("@MBX store 1"),
            Err(LineError::UnknownCommand("store".into()))
        );
    }

    #[test]
    fn store_line() {
        let line = format!(
            "@MBX STORE 7 aabbccdd {} {} eHh4",
            "01".repeat(32),
            "ab".repeat(8)
        );
        assert_eq!(
            parse_line(&line),
            Ok(Some(Request::Store {
                id: 7,
                owner4: Owner4([0xaa, 0xbb, 0xcc, 0xdd]),
                sender: pk(1),
                pkt_hash: PktHash([0xab; 8]),
                payload: b"xxx".to_vec(),
            }))
        );
    }

    #[test]
    fn carriage_return_newline_and_uppercase_hex_accepted() {
        let line = format!(
            "@MBX STORE 0007 AABBCCDD {} {} eHh4\r\n",
            "0A".repeat(32),
            "AB".repeat(8)
        );
        match parse_line(&line) {
            Ok(Some(Request::Store {
                id, owner4, sender, ..
            })) => {
                assert_eq!(
                    (id, owner4, sender),
                    (7, Owner4([0xaa, 0xbb, 0xcc, 0xdd]), pk(10))
                );
            }
            other => panic!("{other:?}"),
        }
    }

    #[test]
    fn reg_line() {
        let line = format!(
            "@MBX REG 4294967295 {} 01020304 {}",
            "02".repeat(32),
            "0f".repeat(8)
        );
        assert_eq!(
            parse_line(&line),
            Ok(Some(Request::Reg {
                id: u32::MAX,
                sender: pk(2),
                owner4: Owner4([1, 2, 3, 4]),
                token: Token([0x0f; 8])
            }))
        );
    }

    #[test]
    fn fetch_line_with_and_without_reports() {
        let line = format!("@MBX FETCH 3 {} 02 0000abcd -", "03".repeat(32));
        assert_eq!(
            parse_line(&line),
            Ok(Some(Request::Fetch {
                id: 3,
                client: pk(3),
                flags: 2,
                store_id: 0xabcd,
                reports: vec![]
            }))
        );
        let line = format!(
            "@MBX FETCH 3 {} 00 ffffffff {}:0:{},{}:255:{}",
            "03".repeat(32),
            "11".repeat(8),
            "aa".repeat(6),
            "22".repeat(8),
            "bb".repeat(6)
        );
        assert_eq!(
            parse_line(&line),
            Ok(Some(Request::Fetch {
                id: 3,
                client: pk(3),
                flags: 0,
                store_id: u32::MAX,
                reports: vec![
                    Report {
                        pkt_hash: PktHash([0x11; 8]),
                        result: ReportResult::OnRadio,
                        ack: Ack([0xaa; 6])
                    },
                    Report {
                        pkt_hash: PktHash([0x22; 8]),
                        result: ReportResult::Other(255),
                        ack: Ack([0xbb; 6])
                    },
                ],
            }))
        );
    }

    #[test]
    fn eight_reports_and_hashes_fit() {
        let reports: Vec<String> = (0..8)
            .map(|i| format!("{i:016x}:1:{}", "00".repeat(6)))
            .collect();
        let line = format!("@MBX FETCH 1 {Z32} 00 00000001 {}", reports.join(","));
        assert!(
            matches!(parse_line(&line), Ok(Some(Request::Fetch { reports, .. })) if reports.len() == 8)
        );
        let hashes: Vec<String> = (0..8).map(|i| format!("{i:016x}")).collect();
        let line = format!("@MBX STAT 1 {Z32} {}", hashes.join(","));
        assert!(
            matches!(parse_line(&line), Ok(Some(Request::Stat { hashes, .. })) if hashes.len() == 8)
        );
    }

    #[test]
    fn stat_line() {
        let line = format!(
            "@MBX STAT 9 {} {},{}",
            "04".repeat(32),
            "01".repeat(8),
            "02".repeat(8)
        );
        assert_eq!(
            parse_line(&line),
            Ok(Some(Request::Stat {
                id: 9,
                sender: pk(4),
                hashes: vec![PktHash([1; 8]), PktHash([2; 8])]
            }))
        );
    }

    #[test]
    fn hello_takes_any_fields() {
        assert_eq!(
            parse_line("@MBX HELLO"),
            Ok(Some(Request::Hello {
                firmware: String::new()
            }))
        );
        assert_eq!(
            parse_line("@MBX HELLO 1.0-rdm extra\r"),
            Ok(Some(Request::Hello {
                firmware: "1.0-rdm extra".into()
            }))
        );
    }

    #[test]
    fn time_line() {
        assert_eq!(
            parse_line("@MBX TIME 1790000000"),
            Ok(Some(Request::Time {
                unix: 1_790_000_000
            }))
        );
        assert_eq!(
            parse_line("@MBX TIME  5 \r\n"),
            Ok(Some(Request::Time { unix: 5 }))
        );
        for bad in [
            "@MBX TIME",
            "@MBX TIME ",
            "@MBX TIME x",
            "@MBX TIME 4294967296",
            "@MBX TIME 5 6",
        ] {
            assert!(malformed(bad), "{bad:?}");
        }
    }

    // test_mbxd.py test_malformed_lines_get_no_reply, plus the base64 edge cases of Python's strict decoder.
    #[test]
    fn malformed_lines() {
        let st = |rid: &str, o4: &str, sender: &str, hash: &str, b64: &str| {
            format!("@MBX STORE {rid} {o4} {sender} {hash} {b64}")
        };
        let nine_reports: Vec<String> = (0..9)
            .map(|i| format!("{i:016x}:0:{}", "00".repeat(6)))
            .collect();
        let nine_hashes: Vec<String> = (0..9).map(|i| format!("{i:016x}")).collect();
        let lines = vec![
            "@MBX STORE 1 aabbccdd".to_string(),
            st("x", "aabbccdd", Z32, Z8, "AAAA"),
            st("4294967296", "aabbccdd", Z32, Z8, "AAAA"),
            st("-1", "aabbccdd", Z32, Z8, "AAAA"),
            st("+1", "aabbccdd", Z32, Z8, "AAAA"),
            st("12345678901", "aabbccdd", Z32, Z8, "AAAA"),
            st("", "aabbccdd", Z32, Z8, "AAAA"),
            st("1", "aabbcc", Z32, Z8, "AAAA"),
            st("1", "aabbccdd", &"00".repeat(31), Z8, "AAAA"),
            st("1", "aabbccdd", &"zz".repeat(32), Z8, "AAAA"),
            st("1", "aabbccdd", Z32, "0000000", "AAAA"),
            st("1", "aabbccdd", Z32, Z8, "AAA"),
            st("1", "aabbccdd", Z32, Z8, "A*AA"),
            st("1", "aabbccdd", Z32, Z8, ""),
            st("1", "aabbccdd", Z32, Z8, "AAAA=="),
            st("1", "aabbccdd", Z32, Z8, "A==="),
            st("1", "aabbccdd", Z32, Z8, "=="),
            st("1", "aabbccdd", Z32, Z8, "AA=A"),
            st("1", "aabbccdd", Z32, Z8, "AA==AA=="),
            st("1", "aabbccdd", Z32, Z8, "AAAA\t"),
            format!("@MBX STORE 1  aabbccdd {Z32} {Z8} AAAA"),
            format!("@MBX STORE 1 aabbccdd {Z32} {Z8} AAAA extra"),
            format!("@MBX STORE 1 aabbccdd {Z32} {Z8} AAAA "),
            format!("@MBX REG 1 {Z32} aabbccdd {}", "00".repeat(7)),
            format!("@MBX FETCH 1 {Z32} 0 00000001 -"),
            format!("@MBX FETCH 1 {Z32} 00 0001 -"),
            format!("@MBX FETCH 1 {Z32} 00 00000001 "),
            format!("@MBX FETCH 1 {Z32} 00 00000001 {Z8}:0"),
            format!("@MBX FETCH 1 {Z32} 00 00000001 {Z8}:x:{}", "00".repeat(6)),
            format!("@MBX FETCH 1 {Z32} 00 00000001 {Z8}:256:{}", "00".repeat(6)),
            format!("@MBX FETCH 1 {Z32} 00 00000001 {Z8}:0:{}:1", "00".repeat(6)),
            format!("@MBX FETCH 1 {Z32} 00 00000001 {Z8}:0:{},", "00".repeat(6)),
            format!("@MBX FETCH 1 {Z32} 00 00000001 {}", nine_reports.join(",")),
            format!("@MBX STAT 1 {Z32}"),
            format!("@MBX STAT 1 {Z32} -"),
            format!("@MBX STAT 1 {Z32} {}", nine_hashes.join(",")),
            format!("@MBX STAT 1 {Z32} {Z8},"),
        ];
        for line in &lines {
            assert!(malformed(line), "{line:?} -> {:?}", parse_line(line));
        }
    }

    #[test]
    fn base64_accepts_what_python_strict_mode_accepts() {
        let cases: [(&str, &[u8]); 5] = [
            ("AAAA", &[0, 0, 0]),
            ("AA==", &[0]),
            ("AAA=", &[0, 0]),
            ("AB==", &[0]),
            ("Zm8=", b"fo"),
        ];
        for (b64, want) in cases {
            let line = format!("@MBX STORE 1 aabbccdd {Z32} {Z8} {b64}");
            match parse_line(&line) {
                Ok(Some(Request::Store { payload, .. })) => assert_eq!(payload, want, "{b64}"),
                other => panic!("{b64}: {other:?}"),
            }
        }
    }

    #[test]
    fn replies_format_like_mbxd_py() {
        let cases = vec![
            (
                Reply::Acl {
                    pubkey: pk(0xab),
                    role: Role::Owner,
                    owner4: Owner4([0xab; 4]),
                },
                format!("mbx.acl {} o abababab", "ab".repeat(32)),
            ),
            (
                Reply::Acl {
                    pubkey: pk(1),
                    role: Role::Depositor,
                    owner4: Owner4([2; 4]),
                },
                format!("mbx.acl {} d 02020202", "01".repeat(32)),
            ),
            (Reply::Ready, "mbx.ready".into()),
            (Reply::Time(1_790_000_000), "mbx.time 1790000000".into()),
            (
                Reply::Store {
                    id: 5,
                    code: Code::Ok,
                    expires: 1_790_604_800,
                },
                "mbx.store 5 00 1790604800".into(),
            ),
            (
                Reply::Store {
                    id: 5,
                    code: Code::NoStorage,
                    expires: 0,
                },
                "mbx.store 5 13 0".into(),
            ),
            (
                Reply::Reg {
                    id: 1,
                    code: Code::NotAuth,
                    ttl_days: 0,
                    quota: 0,
                },
                "mbx.reg 1 10 0 0".into(),
            ),
            (
                Reply::Reg {
                    id: 1,
                    code: Code::Ok,
                    ttl_days: 7,
                    quota: 20,
                },
                "mbx.reg 1 00 7 20".into(),
            ),
            (
                Reply::Fetch {
                    id: 3,
                    code: Code::Ok,
                    remaining: 0,
                    reports_ok: 0,
                    payload: Some((0u8..41).collect()),
                },
                "mbx.fetch 3 00 0 0 AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8gISIjJCUmJyg="
                    .into(),
            ),
            (
                Reply::Fetch {
                    id: 6,
                    code: Code::NoStorage,
                    remaining: 0,
                    reports_ok: 0,
                    payload: None,
                },
                "mbx.fetch 6 13 0 0 -".into(),
            ),
            (
                Reply::Fetch {
                    id: 6,
                    code: Code::Ok,
                    remaining: 2,
                    reports_ok: 1,
                    payload: Some(vec![]),
                },
                "mbx.fetch 6 00 2 1 -".into(),
            ),
            (
                Reply::Stat {
                    id: 7,
                    code: Code::Ok,
                    items: vec![
                        StatItem {
                            state: 2,
                            ack: Ack([0xaa, 0xaa, 0xaa, 0xaa, 0, 0]),
                        },
                        StatItem {
                            state: 0,
                            ack: Ack::NONE,
                        },
                    ],
                },
                "mbx.stat 7 00 2:aaaaaaaa0000,0:000000000000".into(),
            ),
            (
                Reply::Stat {
                    id: 7,
                    code: Code::NoStorage,
                    items: vec![],
                },
                "mbx.stat 7 13 -".into(),
            ),
        ];
        for (reply, line) in cases {
            assert_eq!(reply.to_string(), line);
        }
    }

    #[test]
    fn requests_format_as_the_radio_sends_them() {
        let req = Request::Fetch {
            id: 2,
            client: pk(0xcd),
            flags: 2,
            store_id: 0xabcd,
            reports: vec![Report {
                pkt_hash: PktHash([1; 8]),
                result: ReportResult::Synced,
                ack: Ack([0xbb; 6]),
            }],
        };
        assert_eq!(
            req.to_string(),
            format!(
                "@MBX FETCH 2 {} 02 0000abcd {}:1:{}",
                "cd".repeat(32),
                "01".repeat(8),
                "bb".repeat(6)
            )
        );
        let req = Request::Fetch {
            id: 2,
            client: pk(0xcd),
            flags: 0,
            store_id: 1,
            reports: vec![],
        };
        assert_eq!(
            req.to_string(),
            format!("@MBX FETCH 2 {} 00 00000001 -", "cd".repeat(32))
        );
        assert_eq!(
            Request::Hello {
                firmware: "fw".into()
            }
            .to_string(),
            "@MBX HELLO fw"
        );
        assert_eq!(Request::Time { unix: 5 }.to_string(), "@MBX TIME 5");
    }

    #[test]
    fn command_line_arguments() {
        assert_eq!(parse_owner("0A0B0C0D"), Ok(Owner4([10, 11, 12, 13])));
        assert_eq!(parse_owner(&"0a".repeat(32)), Ok(Owner4([10; 4])));
        assert!(parse_owner("0a0b0c").is_err());
        assert!(parse_owner(&"zz".repeat(32)).is_err());
        assert_eq!(parse_pubkey(&"ff".repeat(32)), Ok(pk(0xff)));
        assert!(parse_pubkey("ff").is_err());
        assert_eq!(parse_k_owner(&"11".repeat(16)), Ok(KOwner([0x11; 16])));
        assert!(parse_k_owner(&"11".repeat(15)).is_err());
    }
}
