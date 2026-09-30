//! The mailbox rules that need no storage (README, "Message states" and "Rules per request").

use hmac::{KeyInit, Mac};

use super::model::{DAY, Limits, Message, Owner};
use crate::types::{
    Ack, KOwner, Owner4, Pubkey, Report, ReportResult, Role, State, Token, UnixTime,
};

/// Final states are kept this long, so a sender's STATUS still gets an answer.
pub const RETAIN_S: UnixTime = 30 * DAY;
/// Largest inner payload: a TXT_MSG that still fits a FETCH reply.
pub const MAX_INNER: usize = 164;
/// Size of the radio's peer cache, filled after HELLO.
pub const ACL_MAX: usize = 64;
/// `mbx.time` interval after HELLO.
pub const TIME_INTERVAL_S: UnixTime = 600;
/// States that count against the quota.
pub const LIVE: [State; 3] = [State::Stored, State::Sent, State::OnRadio];

type HmacSha256 = hmac::Hmac<sha2::Sha256>;

fn keyed(k_owner: &KOwner, sender: &Pubkey) -> HmacSha256 {
    // HMAC zero-pads a key shorter than the hash block (RFC 2104), so this is HMAC keyed with the 16 bytes.
    let mut key = hmac::digest::Key::<HmacSha256>::default();
    key[..KOwner::LEN].copy_from_slice(&k_owner.0);
    let mut mac = HmacSha256::new(&key);
    mac.update(&sender.0);
    mac
}

/// `token_B = HMAC-SHA256(K_owner, sender)[0:8]`, what the owner's radio hands a depositor.
pub fn token(k_owner: &KOwner, sender: &Pubkey) -> Token {
    let tag = keyed(k_owner, sender).finalize().into_bytes();
    let mut t = [0u8; Token::LEN];
    t.copy_from_slice(&tag[..Token::LEN]);
    Token(t)
}

/// Constant-time check of a registration token.
pub fn token_valid(k_owner: &KOwner, sender: &Pubkey, token: &Token) -> bool {
    keyed(k_owner, sender)
        .verify_truncated_left(&token.0)
        .is_ok()
}

/// Which deadline a timer rule watches.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Deadline {
    TRadio,
    TSync,
}

/// A timer: messages in `from` whose `deadline` has passed become `to`, final from the deadline on.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Expiry {
    pub from: &'static [State],
    pub deadline: Deadline,
    pub to: State,
}

/// Run at the start of every request, at the request's time.
pub const EXPIRIES: [Expiry; 2] = [
    Expiry {
        from: &[State::Stored, State::Sent],
        deadline: Deadline::TRadio,
        to: State::Expired,
    },
    Expiry {
        from: &[State::OnRadio],
        deadline: Deadline::TSync,
        to: State::SyncExpired,
    },
];

impl Expiry {
    /// The message after this timer, if it ran out at or before `now`.
    pub fn apply(&self, m: &Message, now: UnixTime) -> Option<Message> {
        let deadline = match self.deadline {
            Deadline::TRadio => Some(m.t_radio),
            Deadline::TSync => m.t_sync,
        }?;
        (self.from.contains(&m.state) && deadline <= now).then(|| finalized(m, self.to, deadline))
    }
}

/// Rows in a final state are removed this long after `final_at`.
pub fn purge_until(now: UnixTime) -> UnixTime {
    now - RETAIN_S
}

fn finalized(m: &Message, state: State, at: UnixTime) -> Message {
    Message {
        state,
        payload: None,
        final_at: Some(at),
        ..m.clone()
    }
}

/// The message after one of the owner's reports, or `None` when the report does not fit its state (it is still
/// confirmed, so the owner can drop it).
pub fn after_report(
    m: &Message,
    report: &Report,
    now: UnixTime,
    limits: &Limits,
) -> Option<Message> {
    use State::*;
    match (report.result, m.state) {
        (ReportResult::OnRadio | ReportResult::Duplicate, Stored | Sent) => Some(Message {
            state: OnRadio,
            ack_r: Some(report.ack),
            t_sync: Some(now + limits.sync_s()),
            ..m.clone()
        }),
        (ReportResult::Synced, Stored | Sent | OnRadio | SyncExpired) => Some(Message {
            ack_s: Some(report.ack),
            ..finalized(m, Delivered, now)
        }),
        (ReportResult::Undecryptable, Stored | Sent) => Some(finalized(m, Rejected, now)),
        (ReportResult::InboxFull, Sent) => Some(Message {
            state: Stored,
            ..m.clone()
        }),
        _ => None,
    }
}

/// The owner's radio lost its store (other `store_id`): the copy goes out again with a fresh T_radio, since
/// T_radio is about reaching her radio and that had already happened once (G12).
pub fn resynced(m: &Message, now: UnixTime, limits: &Limits) -> Message {
    Message {
        state: State::Stored,
        t_radio: now + limits.ttl_s(),
        t_sync: None,
        ack_r: None,
        ..m.clone()
    }
}

/// States a resync puts back to STORED; final states stay.
pub const RESYNC_FROM: [State; 2] = [State::OnRadio, State::Sent];

/// Senders take turns: the first after `last` in pubkey order, wrapping around. `senders` is ascending.
pub fn next_sender(senders: &[Pubkey], last: Option<&Pubkey>) -> Option<Pubkey> {
    last.and_then(|last| senders.iter().find(|s| *s > last))
        .or(senders.first())
        .copied()
}

/// One STATUS answer: only the asking sender's own message is visible.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct StatusItem {
    /// `None`: UNKNOWN.
    pub state: Option<State>,
    pub ack: Ack,
}

pub fn status_item(m: Option<&Message>, sender: &Pubkey) -> StatusItem {
    let Some(m) = m.filter(|m| m.sender == *sender) else {
        return StatusItem {
            state: None,
            ack: Ack::NONE,
        };
    };
    let ack = match m.state {
        State::OnRadio | State::SyncExpired => m.ack_r,
        State::Delivered => m.ack_s,
        _ => None,
    };
    StatusItem {
        state: Some(m.state),
        ack: ack.unwrap_or(Ack::NONE),
    }
}

/// A line of the radio's peer cache.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct AclEntry {
    pub pubkey: Pubkey,
    pub role: Role,
    pub owner4: Owner4,
}

/// Owners first, then depositors in the given order (most recent first), one line per public key, at most
/// [`ACL_MAX`].
pub fn acl(owners: &[Owner], depositors: &[(Pubkey, Owner4)]) -> Vec<AclEntry> {
    let owners = owners.iter().map(|o| AclEntry {
        pubkey: o.pubkey,
        role: Role::Owner,
        owner4: o.owner4(),
    });
    let depositors = depositors.iter().map(|&(pubkey, owner4)| AclEntry {
        pubkey,
        role: Role::Depositor,
        owner4,
    });
    let mut seen = std::collections::HashSet::new();
    owners
        .chain(depositors)
        .filter(|e| seen.insert(e.pubkey))
        .take(ACL_MAX)
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::types::PktHash;

    const NOW: UnixTime = 1_790_000_000;

    fn limits() -> Limits {
        Limits::new(7, 30, 20).unwrap()
    }

    fn msg(state: State) -> Message {
        Message {
            state,
            ..Message::new(
                PktHash([1; 8]),
                Owner4([2; 4]),
                Pubkey([3; 32]),
                b"ct".to_vec(),
                NOW - DAY,
                &limits(),
            )
        }
    }

    fn report(result: u8) -> Report {
        Report {
            pkt_hash: PktHash([1; 8]),
            result: result.into(),
            ack: Ack([0xaa; 6]),
        }
    }

    fn hex(s: &str) -> Vec<u8> {
        (0..s.len())
            .step_by(2)
            .map(|i| u8::from_str_radix(&s[i..i + 2], 16).unwrap())
            .collect()
    }

    #[test]
    fn token_matches_a_conformance_vector() {
        let k = KOwner(hex("cb0aaa6155edfbb4729d4853fa5ee2bf").try_into().unwrap());
        let sender = Pubkey(
            hex("ee88dfb37c4ca654c2279979fcd5400fd8ea9539fe252876868798ab06caf14d")
                .try_into()
                .unwrap(),
        );
        let t = Token(hex("dd052b2f712e4782").try_into().unwrap());
        assert_eq!(token(&k, &sender), t);
        assert!(token_valid(&k, &sender, &t));
        assert!(!token_valid(&k, &Pubkey([0; 32]), &t));
        assert!(!token_valid(&KOwner([0; 16]), &sender, &t));
    }

    #[test]
    fn t_radio_expires_stored_and_sent_from_the_deadline() {
        let [radio, _] = EXPIRIES;
        for state in [State::Stored, State::Sent] {
            let m = msg(state);
            assert_eq!(radio.apply(&m, m.t_radio - 1), None);
            let e = radio.apply(&m, m.t_radio + 100).unwrap();
            assert_eq!(
                (e.state, e.payload, e.final_at),
                (State::Expired, None, Some(m.t_radio))
            );
        }
        assert_eq!(radio.apply(&msg(State::OnRadio), NOW + 100 * DAY), None);
    }

    #[test]
    fn t_sync_expires_on_radio_and_keeps_ack_r() {
        let [_, sync] = EXPIRIES;
        let m = after_report(&msg(State::Sent), &report(0), NOW, &limits()).unwrap();
        assert_eq!(sync.apply(&m, NOW + 30 * DAY - 1), None);
        let e = sync.apply(&m, NOW + 30 * DAY).unwrap();
        assert_eq!(
            (e.state, e.payload, e.final_at, e.ack_r),
            (
                State::SyncExpired,
                None,
                Some(NOW + 30 * DAY),
                Some(Ack([0xaa; 6]))
            )
        );
        assert_eq!(
            sync.apply(&msg(State::Stored), NOW + 100 * DAY),
            None,
            "no t_sync before ON_RADIO"
        );
    }

    #[test]
    fn purge_is_thirty_days_after_final() {
        assert_eq!(purge_until(NOW), NOW - 30 * DAY);
    }

    // The report table of the README, every result against every state.
    #[test]
    fn report_transitions() {
        use State::*;
        let table: [(u8, &[State], State); 5] = [
            (0, &[Stored, Sent], OnRadio),
            (2, &[Stored, Sent], OnRadio),
            (1, &[Stored, Sent, OnRadio, SyncExpired], Delivered),
            (3, &[Stored, Sent], Rejected),
            (4, &[Sent], Stored),
        ];
        for (result, from, to) in table {
            for state in State::ALL {
                let got =
                    after_report(&msg(state), &report(result), NOW, &limits()).map(|m| m.state);
                let want = from.contains(&state).then_some(to);
                assert_eq!(got, want, "result {result} in {state:?}");
            }
        }
        for state in State::ALL {
            assert_eq!(after_report(&msg(state), &report(5), NOW, &limits()), None);
            assert_eq!(
                after_report(&msg(state), &report(255), NOW, &limits()),
                None
            );
        }
    }

    #[test]
    fn on_radio_keeps_ack_r_and_starts_t_sync() {
        let m = after_report(
            &msg(State::Stored),
            &report(2),
            NOW,
            &Limits::new(7, 10, 20).unwrap(),
        )
        .unwrap();
        assert_eq!(
            (m.ack_r, m.t_sync, m.payload.is_some()),
            (Some(Ack([0xaa; 6])), Some(NOW + 10 * DAY), true)
        );
    }

    #[test]
    fn delivered_and_rejected_wipe_the_payload() {
        let d = after_report(&msg(State::OnRadio), &report(1), NOW, &limits()).unwrap();
        assert_eq!(
            (d.payload, d.ack_s, d.final_at),
            (None, Some(Ack([0xaa; 6])), Some(NOW))
        );
        let r = after_report(&msg(State::Sent), &report(3), NOW, &limits()).unwrap();
        assert_eq!((r.payload, r.ack_s, r.final_at), (None, None, Some(NOW)));
    }

    #[test]
    fn resync_restarts_t_radio_and_forgets_ack_r() {
        let on_radio = after_report(&msg(State::Sent), &report(0), NOW, &limits()).unwrap();
        let m = resynced(&on_radio, NOW + DAY, &Limits::new(3, 30, 20).unwrap());
        assert_eq!(
            (m.state, m.t_radio, m.t_sync, m.ack_r),
            (State::Stored, NOW + DAY + 3 * DAY, None, None)
        );
        assert!(m.payload.is_some());
    }

    #[test]
    fn senders_take_turns_in_pubkey_order() {
        let (a, b, c) = (Pubkey([1; 32]), Pubkey([2; 32]), Pubkey([3; 32]));
        assert_eq!(next_sender(&[], None), None);
        assert_eq!(next_sender(&[], Some(&a)), None);
        assert_eq!(next_sender(&[a, b, c], None), Some(a));
        assert_eq!(next_sender(&[a, b, c], Some(&a)), Some(b));
        assert_eq!(
            next_sender(&[a, c], Some(&b)),
            Some(c),
            "a sender without copies is skipped"
        );
        assert_eq!(next_sender(&[a, b, c], Some(&c)), Some(a), "wraps around");
        assert_eq!(
            next_sender(&[b], Some(&b)),
            Some(b),
            "one sender keeps its turn"
        );
    }

    #[test]
    fn status_shows_only_own_messages() {
        let on_radio = after_report(&msg(State::Sent), &report(0), NOW, &limits()).unwrap();
        let me = on_radio.sender;
        assert_eq!(
            status_item(None, &me),
            StatusItem {
                state: None,
                ack: Ack::NONE
            }
        );
        assert_eq!(
            status_item(Some(&on_radio), &Pubkey([9; 32])),
            StatusItem {
                state: None,
                ack: Ack::NONE
            }
        );
        assert_eq!(
            status_item(Some(&on_radio), &me),
            StatusItem {
                state: Some(State::OnRadio),
                ack: Ack([0xaa; 6])
            }
        );
        let delivered = after_report(
            &on_radio,
            &Report {
                ack: Ack([0xbb; 6]),
                ..report(1)
            },
            NOW,
            &limits(),
        )
        .unwrap();
        assert_eq!(
            status_item(Some(&delivered), &me).ack,
            Ack([0xbb; 6]),
            "ACK_S for DELIVERED"
        );
        let sync_expired = EXPIRIES[1].apply(&on_radio, NOW + 100 * DAY).unwrap();
        assert_eq!(
            status_item(Some(&sync_expired), &me).ack,
            Ack([0xaa; 6]),
            "ACK_R for SYNC_EXPIRED"
        );
        assert_eq!(
            status_item(Some(&msg(State::Sent)), &me),
            StatusItem {
                state: Some(State::Sent),
                ack: Ack::NONE
            }
        );
    }

    fn owner(b: u8) -> Owner {
        Owner {
            pubkey: Pubkey([b; 32]),
            k_owner: KOwner([0; 16]),
            limits: limits(),
            store_id: None,
            last_sender: None,
        }
    }

    #[test]
    fn acl_owners_first_then_depositors_once_each() {
        let o1 = owner(1);
        let d = |b: u8, o: u8| (Pubkey([b; 32]), Owner4([o; 4]));
        let got = acl(
            std::slice::from_ref(&o1),
            &[d(5, 1), d(1, 1), d(5, 2), d(6, 1)],
        );
        let want = vec![
            AclEntry {
                pubkey: o1.pubkey,
                role: Role::Owner,
                owner4: o1.owner4(),
            },
            AclEntry {
                pubkey: Pubkey([5; 32]),
                role: Role::Depositor,
                owner4: Owner4([1; 4]),
            },
            AclEntry {
                pubkey: Pubkey([6; 32]),
                role: Role::Depositor,
                owner4: Owner4([1; 4]),
            },
        ];
        assert_eq!(got, want);
    }

    #[test]
    fn acl_is_capped() {
        let owners: Vec<Owner> = (0..3).map(owner).collect();
        let depositors: Vec<(Pubkey, Owner4)> = (10..=200)
            .map(|b| (Pubkey([b; 32]), Owner4([0; 4])))
            .collect();
        let got = acl(&owners, &depositors);
        assert_eq!(got.len(), ACL_MAX);
        assert_eq!(got[3].pubkey, Pubkey([10; 32]));
    }
}
