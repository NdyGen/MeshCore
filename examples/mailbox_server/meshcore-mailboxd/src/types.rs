//! Values shared by the line protocol and the mailbox rules. No behaviour beyond conversions.

use std::fmt;

/// Seconds since the Unix epoch on the mailbox's clock. Signed and 64-bit: deadlines are sums of a `u32` clock
/// and days, and SQLite stores them as `INTEGER`.
pub type UnixTime = i64;

/// Request id chosen by the radio, echoed in the reply.
pub type RequestId = u32;

macro_rules! bytes_value {
    ($(#[$doc:meta])* $name:ident, $len:expr) => {
        $(#[$doc])*
        #[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash)]
        pub struct $name(pub [u8; $len]);

        impl $name {
            pub const LEN: usize = $len;

            pub fn from_slice(bytes: &[u8]) -> Option<Self> {
                bytes.try_into().ok().map(Self)
            }

            pub fn as_bytes(&self) -> &[u8] {
                &self.0
            }
        }

        impl fmt::Display for $name {
            fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
                self.0.iter().try_for_each(|b| write!(f, "{b:02x}"))
            }
        }

        impl fmt::Debug for $name {
            fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
                write!(f, concat!(stringify!($name), "({})"), self)
            }
        }
    };
}

bytes_value!(
    /// Ed25519 public key of an owner or depositor.
    Pubkey,
    32
);
bytes_value!(
    /// First four bytes of an owner's public key: how the owner is named on the wire.
    Owner4,
    4
);
bytes_value!(
    /// Packet hash of a deposited message; unique across the store.
    PktHash,
    8
);
bytes_value!(
    /// ACK_R or ACK_S, padded to six bytes.
    Ack,
    6
);
bytes_value!(
    /// `token_B = HMAC-SHA256(K_owner, sender)[0:8]`.
    Token,
    8
);
bytes_value!(
    /// The per-owner secret shared with the owner's radio.
    KOwner,
    16
);

impl Pubkey {
    pub fn owner4(&self) -> Owner4 {
        Owner4([self.0[0], self.0[1], self.0[2], self.0[3]])
    }
}

impl Ack {
    pub const NONE: Ack = Ack([0; 6]);
}

/// Status code of a reply (`03` par. 3).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum Code {
    Ok = 0x00,
    AlreadyStored = 0x01,
    NotAuth = 0x10,
    UnknownOwner = 0x11,
    Quota = 0x12,
    NoStorage = 0x13,
    TooBig = 0x14,
}

impl Code {
    pub fn byte(self) -> u8 {
        self as u8
    }
}

/// Result of one FETCH report. Values without a meaning are accepted on the wire and change nothing.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ReportResult {
    OnRadio,
    Synced,
    Duplicate,
    Undecryptable,
    InboxFull,
    Other(u8),
}

impl From<u8> for ReportResult {
    fn from(v: u8) -> Self {
        match v {
            0 => Self::OnRadio,
            1 => Self::Synced,
            2 => Self::Duplicate,
            3 => Self::Undecryptable,
            4 => Self::InboxFull,
            v => Self::Other(v),
        }
    }
}

impl From<ReportResult> for u8 {
    fn from(r: ReportResult) -> u8 {
        match r {
            ReportResult::OnRadio => 0,
            ReportResult::Synced => 1,
            ReportResult::Duplicate => 2,
            ReportResult::Undecryptable => 3,
            ReportResult::InboxFull => 4,
            ReportResult::Other(v) => v,
        }
    }
}

/// Alice's report on one copy, carried in her next FETCH.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Report {
    pub pkt_hash: PktHash,
    pub result: ReportResult,
    pub ack: Ack,
}

/// State of a deposited message. `Sent` is internal: on the wire it reads as STORED.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum State {
    Stored,
    Sent,
    OnRadio,
    Delivered,
    Rejected,
    Expired,
    SyncExpired,
}

impl State {
    pub const ALL: [State; 7] = [
        State::Stored,
        State::Sent,
        State::OnRadio,
        State::Delivered,
        State::Rejected,
        State::Expired,
        State::SyncExpired,
    ];

    /// Wire value for STATUS replies; 0 (UNKNOWN) is for hashes the sender has no message for.
    pub fn wire(self) -> u8 {
        match self {
            State::Stored | State::Sent => 1,
            State::OnRadio => 2,
            State::Delivered => 3,
            State::Rejected => 4,
            State::Expired => 5,
            State::SyncExpired => 6,
        }
    }

    /// The `messages.state` column value (the database predates this daemon; the names are kept).
    pub fn db_name(self) -> &'static str {
        match self {
            State::Stored => "stored",
            State::Sent => "sent",
            State::OnRadio => "on_radio",
            State::Delivered => "delivered",
            State::Rejected => "rejected",
            State::Expired => "expired",
            State::SyncExpired => "sync_expired",
        }
    }

    pub fn from_db_name(s: &str) -> Option<State> {
        State::ALL.into_iter().find(|st| st.db_name() == s)
    }
}

/// Role of a public key in the radio's peer cache.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Role {
    Owner,
    Depositor,
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn bytes_values_print_lowercase_hex() {
        assert_eq!(Owner4([0xAB, 0x01, 0xff, 0x00]).to_string(), "ab01ff00");
        assert_eq!(
            format!("{:?}", Ack([1, 2, 3, 4, 5, 6])),
            "Ack(010203040506)"
        );
    }

    #[test]
    fn from_slice_checks_length() {
        assert_eq!(
            Owner4::from_slice(&[1, 2, 3, 4]),
            Some(Owner4([1, 2, 3, 4]))
        );
        assert_eq!(Owner4::from_slice(&[1, 2, 3]), None);
    }

    #[test]
    fn owner4_is_the_pubkey_prefix() {
        let mut pk = [0u8; 32];
        pk[..5].copy_from_slice(&[9, 8, 7, 6, 5]);
        assert_eq!(Pubkey(pk).owner4(), Owner4([9, 8, 7, 6]));
    }

    #[test]
    fn pubkeys_order_bytewise_like_sqlite_blobs() {
        let mut a = [0u8; 32];
        let mut b = [0u8; 32];
        a[0] = 0x7f;
        b[0] = 0x80;
        assert!(Pubkey(a) < Pubkey(b));
    }

    #[test]
    fn report_results_roundtrip() {
        for v in 0..=255u8 {
            assert_eq!(u8::from(ReportResult::from(v)), v);
        }
        assert_eq!(ReportResult::from(4), ReportResult::InboxFull);
        assert_eq!(ReportResult::from(5), ReportResult::Other(5));
    }

    #[test]
    fn state_names_and_wire_values_match_mbxd_py() {
        let expect = [
            ("stored", 1),
            ("sent", 1),
            ("on_radio", 2),
            ("delivered", 3),
            ("rejected", 4),
            ("expired", 5),
            ("sync_expired", 6),
        ];
        for (st, (name, wire)) in State::ALL.into_iter().zip(expect) {
            assert_eq!((st.db_name(), st.wire()), (name, wire));
            assert_eq!(State::from_db_name(name), Some(st));
        }
        assert_eq!(State::from_db_name("STORED"), None);
    }

    #[test]
    fn codes_match_the_wire() {
        let codes = [
            (Code::Ok, 0x00),
            (Code::AlreadyStored, 0x01),
            (Code::NotAuth, 0x10),
            (Code::UnknownOwner, 0x11),
            (Code::Quota, 0x12),
            (Code::NoStorage, 0x13),
            (Code::TooBig, 0x14),
        ];
        for (c, b) in codes {
            assert_eq!(c.byte(), b);
        }
    }
}
