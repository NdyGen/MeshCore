//! One radio session: a line from the radio in, its replies out. A reply exists only after its transaction
//! committed; a storage failure turns into NO_STORAGE, a malformed line into no reply at all (the radio times
//! out after 3 s and reports NO_STORAGE itself).

use log::{error, info, warn};

use crate::domain::policy::TIME_INTERVAL_S;
use crate::domain::usecases::{Deposit, FetchOutcome, FetchRequest, RegOutcome, StoreOutcome};
use crate::mailbox::Mailbox;
use crate::protocol::{self, FETCH_FLAG_NO_PAYLOAD, LineError, Reply, Request, StatItem};
use crate::storage::{Storage, StorageError};
use crate::system::Clock;
use crate::types::{Code, UnixTime};

/// When `mbx.time` is due: at HELLO and every 600 s after, never before the first HELLO.
#[derive(Debug, Default)]
pub struct TimeBeacon {
    last: Option<UnixTime>,
}

impl TimeBeacon {
    pub fn start(&mut self, now: UnixTime) -> Reply {
        self.last = Some(now);
        Reply::Time(now)
    }

    pub fn due(&mut self, now: UnixTime) -> Option<Reply> {
        let last = self.last?;
        (now >= last + TIME_INTERVAL_S).then(|| self.start(now))
    }
}

pub struct Session<S: Storage, C: Clock> {
    mailbox: Mailbox<S>,
    clock: C,
    beacon: TimeBeacon,
}

fn clip(line: &str) -> String {
    line.chars().take(120).collect()
}

/// The answer when the transaction did not commit: nothing changed, and the radio hears NO_STORAGE.
fn or_no_storage<T>(result: Result<T, StorageError>, fallback: T) -> T {
    result.unwrap_or_else(|e| {
        error!("storage error, answering NO_STORAGE: {e}");
        fallback
    })
}

impl<S: Storage, C: Clock> Session<S, C> {
    pub fn new(mailbox: Mailbox<S>, clock: C) -> Self {
        Session {
            mailbox,
            clock,
            beacon: TimeBeacon::default(),
        }
    }

    pub fn mailbox(&self) -> &Mailbox<S> {
        &self.mailbox
    }

    pub fn mailbox_mut(&mut self) -> &mut Mailbox<S> {
        &mut self.mailbox
    }

    /// The replies to one line. An error means the session cannot go on (the ACL for HELLO is unreadable).
    pub fn handle_line(&mut self, line: &str) -> Result<Vec<Reply>, StorageError> {
        let request = match protocol::parse_line(line) {
            Ok(Some(request)) => request,
            Ok(None) => return Ok(Vec::new()),
            Err(LineError::UnknownCommand(_)) => {
                warn!("unknown command: {}", clip(line));
                return Ok(Vec::new());
            }
            Err(e) => {
                warn!("malformed line ignored ({e}): {}", clip(line));
                return Ok(Vec::new());
            }
        };
        let now = self.clock.now();
        let reply = match request {
            Request::Hello { firmware } => return self.hello(&firmware, now),
            Request::Time { unix } => {
                if !self.clock.set(unix.into()) {
                    warn!("unknown command: {}", clip(line));
                }
                return Ok(Vec::new());
            }
            Request::Store {
                id,
                owner4,
                sender,
                pkt_hash,
                payload,
            } => {
                let deposit = Deposit {
                    owner4,
                    sender,
                    pkt_hash,
                    payload: &payload,
                };
                let r = or_no_storage(
                    self.mailbox.store(now, &deposit),
                    StoreOutcome {
                        code: Code::NoStorage,
                        expires: 0,
                    },
                );
                Reply::Store {
                    id,
                    code: r.code,
                    expires: r.expires,
                }
            }
            Request::Reg {
                id,
                sender,
                owner4,
                token,
            } => {
                let fallback = RegOutcome {
                    code: Code::NoStorage,
                    ttl_days: 0,
                    quota: 0,
                };
                let r = or_no_storage(
                    self.mailbox.register(now, &sender, &owner4, &token),
                    fallback,
                );
                Reply::Reg {
                    id,
                    code: r.code,
                    ttl_days: r.ttl_days,
                    quota: r.quota,
                }
            }
            Request::Fetch {
                id,
                client,
                flags,
                store_id,
                reports,
            } => {
                let req = FetchRequest {
                    client,
                    no_payload: flags & FETCH_FLAG_NO_PAYLOAD != 0,
                    store_id,
                    reports: &reports,
                };
                let fallback = FetchOutcome {
                    code: Code::NoStorage,
                    remaining: 0,
                    reports_ok: 0,
                    payload: None,
                    resync: None,
                    ignored: Vec::new(),
                };
                let r = or_no_storage(self.mailbox.fetch(now, &req), fallback);
                if let Some(rs) = r.resync {
                    info!(
                        "owner {} store_id {:08x} -> {:08x}, {} messages back to stored",
                        rs.owner4, rs.from, rs.to, rs.requeued
                    );
                }
                for ig in &r.ignored {
                    match ig.state {
                        None => info!("report for unknown hash {}", ig.pkt_hash),
                        Some(st) => info!(
                            "report {} ignored for {} in state {}",
                            u8::from(ig.result),
                            ig.pkt_hash,
                            st.db_name()
                        ),
                    }
                }
                Reply::Fetch {
                    id,
                    code: r.code,
                    remaining: r.remaining,
                    reports_ok: r.reports_ok,
                    payload: r.payload,
                }
            }
            Request::Stat { id, sender, hashes } => {
                match self.mailbox.status(now, &sender, &hashes) {
                    Ok(items) => Reply::Stat {
                        id,
                        code: Code::Ok,
                        items: items
                            .iter()
                            .map(|i| StatItem {
                                state: i.state.map_or(0, |s| s.wire()),
                                ack: i.ack,
                            })
                            .collect(),
                    },
                    Err(e) => or_no_storage(
                        Err(e),
                        Reply::Stat {
                            id,
                            code: Code::NoStorage,
                            items: Vec::new(),
                        },
                    ),
                }
            }
        };
        Ok(vec![reply])
    }

    /// The radio (re)booted: fill its peer cache, then `mbx.ready` and the time.
    fn hello(&mut self, firmware: &str, now: UnixTime) -> Result<Vec<Reply>, StorageError> {
        info!(
            "radio hello, firmware {}",
            if firmware.is_empty() { "?" } else { firmware }
        );
        let time = self.beacon.start(now);
        let mut replies: Vec<Reply> = self
            .mailbox
            .acl()?
            .into_iter()
            .map(|e| Reply::Acl {
                pubkey: e.pubkey,
                role: e.role,
                owner4: e.owner4,
            })
            .collect();
        replies.extend([Reply::Ready, time]);
        Ok(replies)
    }

    /// A due `mbx.time`, checked after every line and whenever the radio is quiet.
    pub fn tick(&mut self) -> Option<Reply> {
        self.beacon.due(self.clock.now())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn beacon_is_silent_before_hello() {
        let mut b = TimeBeacon::default();
        assert_eq!(b.due(0), None);
        assert_eq!(b.due(10_000), None);
    }

    #[test]
    fn beacon_repeats_600_s_after_the_last_one() {
        let mut b = TimeBeacon::default();
        assert_eq!(b.start(1000), Reply::Time(1000));
        assert_eq!(b.due(1599), None);
        assert_eq!(b.due(1650), Some(Reply::Time(1650)));
        assert_eq!(b.due(2249), None);
        assert_eq!(b.due(2250), Some(Reply::Time(2250)));
    }

    #[test]
    fn beacon_follows_a_clock_set_back() {
        let mut b = TimeBeacon::default();
        b.start(5000);
        assert_eq!(b.due(100), None);
        assert_eq!(b.due(5600), Some(Reply::Time(5600)));
    }
}
