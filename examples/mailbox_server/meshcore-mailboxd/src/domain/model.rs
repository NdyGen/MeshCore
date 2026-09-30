//! What the mailbox keeps: owners with their limits, and deposited messages.

use crate::types::{Ack, KOwner, Owner4, PktHash, Pubkey, State, UnixTime};

pub const DAY: UnixTime = 86_400;

/// Per-owner limits, validated on the way in.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Limits {
    ttl_days: u8,
    sync_days: u8,
    quota: u8,
}

#[derive(Debug, Clone, PartialEq, Eq, thiserror::Error)]
#[error("ttl_days 1-{max_ttl}, sync_days 1-255, quota 1-255", max_ttl = Limits::MAX_TTL_DAYS)]
pub struct LimitsError;

impl Limits {
    /// T_radio is carried as `ttl_s` and must stay within what a sender accepts.
    pub const MAX_TTL_DAYS: u8 = 30;

    pub fn new(ttl_days: i64, sync_days: i64, quota: i64) -> Result<Self, LimitsError> {
        let in_range = |v: i64, max: u8| u8::try_from(v).ok().filter(|v| (1..=max).contains(v));
        Ok(Limits {
            ttl_days: in_range(ttl_days, Self::MAX_TTL_DAYS).ok_or(LimitsError)?,
            sync_days: in_range(sync_days, u8::MAX).ok_or(LimitsError)?,
            quota: in_range(quota, u8::MAX).ok_or(LimitsError)?,
        })
    }

    /// T_radio: how long a copy may wait to reach the owner's radio.
    pub fn ttl_days(&self) -> u8 {
        self.ttl_days
    }

    /// T_sync: how long an ON_RADIO copy waits for the owner's app to sync.
    pub fn sync_days(&self) -> u8 {
        self.sync_days
    }

    /// Live messages per depositor and owner.
    pub fn quota(&self) -> u8 {
        self.quota
    }

    pub fn ttl_s(&self) -> UnixTime {
        UnixTime::from(self.ttl_days) * DAY
    }

    pub fn sync_s(&self) -> UnixTime {
        UnixTime::from(self.sync_days) * DAY
    }
}

impl Default for Limits {
    fn default() -> Self {
        Limits {
            ttl_days: 7,
            sync_days: 30,
            quota: 20,
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Owner {
    pub pubkey: Pubkey,
    pub k_owner: KOwner,
    pub limits: Limits,
    /// `store_id` of the owner's previous FETCH; a different one means her radio lost its store.
    pub store_id: Option<u32>,
    /// Sender of the copy handed out last, where the FETCH rotation continues.
    pub last_sender: Option<Pubkey>,
}

impl Owner {
    pub fn owner4(&self) -> Owner4 {
        self.pubkey.owner4()
    }
}

/// A deposit: the complete encrypted TXT_MSG payload, never looked into.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Message {
    pub pkt_hash: PktHash,
    pub owner4: Owner4,
    pub sender: Pubkey,
    /// Wiped (`None`) in every final state.
    pub payload: Option<Vec<u8>>,
    pub created: UnixTime,
    /// T_radio deadline, also the `expires` of the DEPOSIT reply.
    pub t_radio: UnixTime,
    /// T_sync deadline, set when the copy reached the owner's radio.
    pub t_sync: Option<UnixTime>,
    pub state: State,
    pub ack_r: Option<Ack>,
    pub ack_s: Option<Ack>,
    /// When the final state was reached; the row goes 30 days later.
    pub final_at: Option<UnixTime>,
}

impl Message {
    pub fn new(
        pkt_hash: PktHash,
        owner4: Owner4,
        sender: Pubkey,
        payload: Vec<u8>,
        now: UnixTime,
        limits: &Limits,
    ) -> Self {
        Message {
            pkt_hash,
            owner4,
            sender,
            payload: Some(payload),
            created: now,
            t_radio: now + limits.ttl_s(),
            t_sync: None,
            state: State::Stored,
            ack_r: None,
            ack_s: None,
            final_at: None,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn limits_ranges() {
        assert!(Limits::new(1, 1, 1).is_ok());
        assert!(Limits::new(30, 255, 255).is_ok());
        for (t, s, q) in [
            (0, 30, 20),
            (31, 30, 20),
            (7, 0, 20),
            (7, 256, 20),
            (7, 30, 0),
            (7, 30, 256),
            (-1, 30, 20),
        ] {
            assert_eq!(Limits::new(t, s, q), Err(LimitsError), "{t} {s} {q}");
        }
        assert_eq!(
            LimitsError.to_string(),
            "ttl_days 1-30, sync_days 1-255, quota 1-255"
        );
    }

    #[test]
    fn default_limits_match_mbxd_py() {
        assert_eq!(Limits::default(), Limits::new(7, 30, 20).unwrap());
    }

    #[test]
    fn a_new_message_is_stored_until_t_radio() {
        let limits = Limits::new(3, 10, 5).unwrap();
        let m = Message::new(
            PktHash([1; 8]),
            Owner4([2; 4]),
            Pubkey([3; 32]),
            vec![9],
            1000,
            &limits,
        );
        assert_eq!(
            (m.state, m.t_radio, m.payload.as_deref()),
            (State::Stored, 1000 + 3 * DAY, Some(&[9u8][..]))
        );
        assert_eq!(
            (m.t_sync, m.ack_r, m.ack_s, m.final_at),
            (None, None, None, None)
        );
    }
}
