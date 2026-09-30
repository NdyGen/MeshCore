//! Property tests for the line parser: malformed input never panics and never parses into a request.

use meshcore_mailboxd::protocol::{MAX_BATCH, Report, Request, parse_line};
use meshcore_mailboxd::types::{Ack, Owner4, PktHash, Pubkey, Token};
use proptest::prelude::*;

fn pubkey() -> impl Strategy<Value = Pubkey> {
    any::<[u8; 32]>().prop_map(Pubkey)
}

fn pkt_hash() -> impl Strategy<Value = PktHash> {
    any::<[u8; 8]>().prop_map(PktHash)
}

fn report() -> impl Strategy<Value = Report> {
    (pkt_hash(), any::<u8>(), any::<[u8; 6]>()).prop_map(|(pkt_hash, r, ack)| Report {
        pkt_hash,
        result: r.into(),
        ack: Ack(ack),
    })
}

fn request() -> impl Strategy<Value = Request> {
    prop_oneof![
        "[ -~]{0,40}".prop_map(|firmware| Request::Hello { firmware }),
        (
            any::<u32>(),
            any::<[u8; 4]>(),
            pubkey(),
            pkt_hash(),
            prop::collection::vec(any::<u8>(), 1..200)
        )
            .prop_map(|(id, o4, sender, pkt_hash, payload)| Request::Store {
                id,
                owner4: Owner4(o4),
                sender,
                pkt_hash,
                payload
            }),
        (any::<u32>(), pubkey(), any::<[u8; 4]>(), any::<[u8; 8]>()).prop_map(
            |(id, sender, o4, t)| Request::Reg {
                id,
                sender,
                owner4: Owner4(o4),
                token: Token(t)
            }
        ),
        (
            any::<u32>(),
            pubkey(),
            any::<u8>(),
            any::<u32>(),
            prop::collection::vec(report(), 0..=MAX_BATCH)
        )
            .prop_map(|(id, client, flags, store_id, reports)| Request::Fetch {
                id,
                client,
                flags,
                store_id,
                reports
            }),
        (
            any::<u32>(),
            pubkey(),
            prop::collection::vec(pkt_hash(), 1..=MAX_BATCH)
        )
            .prop_map(|(id, sender, hashes)| Request::Stat { id, sender, hashes }),
        any::<u32>().prop_map(|unix| Request::Time { unix }),
    ]
}

/// Free-text HELLO and the whitespace-tolerant TIME have no fields to damage.
fn damageable(req: &Request) -> bool {
    !matches!(req, Request::Hello { .. } | Request::Time { .. })
}

/// Damage that leaves a valid line: a longer or shorter decimal id, or base64 twice over.
fn still_valid(req: &Request, field: usize, damage: u8) -> bool {
    field == 2 && damage <= 1 || matches!(req, Request::Store { .. }) && field == 6 && damage == 0
}

proptest! {
    #[test]
    fn arbitrary_text_never_panics(line in "\\PC{0,300}") {
        let _ = parse_line(&line);
    }

    #[test]
    fn arbitrary_request_shaped_text_never_panics(
        cmd in prop::sample::select(vec!["STORE", "REG", "FETCH", "STAT", "HELLO", "TIME"]),
        rest in "[0-9a-fA-F:,+/= \\-xz]{0,300}",
    ) {
        let _ = parse_line(&format!("@MBX {cmd} {rest}"));
    }

    #[test]
    fn requests_roundtrip(req in request()) {
        prop_assert_eq!(parse_line(&req.to_string()), Ok(Some(req.clone())));
        prop_assert_eq!(parse_line(&format!("{req}\r\n")), Ok(Some(req)));
    }

    #[test]
    fn a_damaged_field_is_never_a_request(req in request(), at in any::<prop::sample::Index>(), damage in 0..4u8) {
        prop_assume!(damageable(&req));
        let line = req.to_string();
        let fields: Vec<&str> = line.split(' ').collect();
        let i = 2 + at.index(fields.len() - 2);
        let field = fields[i];
        let bad = match damage {
            0 => format!("{field}{field}"),                  // wrong length
            1 => field[..field.len() - 1].to_string(),       // truncated
            2 => format!("{field} "),                        // extra empty field
            _ => format!("{}\u{e9}", &field[..field.len() - 1]),  // non-ASCII
        };
        prop_assume!(bad != field);
        let mut damaged = fields.clone();
        damaged[i] = &bad;
        let damaged = damaged.join(" ");
        if let Ok(Some(parsed)) = parse_line(&damaged) {
            prop_assert!(still_valid(&req, i, damage), "{damaged:?} -> {parsed:?}");
        }
    }
}
