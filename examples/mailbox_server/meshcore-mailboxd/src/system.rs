//! The outside world the core gets injected: clocks and randomness.

use std::cell::Cell;
use std::rc::Rc;
use std::time::{SystemTime, UNIX_EPOCH};

use crate::domain::usecases::{KeyError, KeySource};
use crate::types::{KOwner, UnixTime};

pub trait Clock {
    fn now(&self) -> UnixTime;

    /// Moves a test clock (`@MBX TIME`); a real clock ignores it and returns false.
    fn set(&self, _t: UnixTime) -> bool {
        false
    }
}

pub struct SystemClock;

impl Clock for SystemClock {
    fn now(&self) -> UnixTime {
        SystemTime::now().duration_since(UNIX_EPOCH).map_or(0, |d| {
            UnixTime::try_from(d.as_secs()).unwrap_or(UnixTime::MAX)
        })
    }
}

/// A clock only the caller moves. Clones share the time, so a test keeps a handle to the session's clock.
#[derive(Debug, Clone, Default)]
pub struct FakeClock(Rc<Cell<UnixTime>>);

impl FakeClock {
    pub fn new(t: UnixTime) -> Self {
        FakeClock(Rc::new(Cell::new(t)))
    }
}

impl Clock for FakeClock {
    fn now(&self) -> UnixTime {
        self.0.get()
    }

    fn set(&self, t: UnixTime) -> bool {
        self.0.set(t);
        true
    }
}

/// `K_owner` from the operating system's random source.
pub struct OsKeys;

impl KeySource for OsKeys {
    fn k_owner(&mut self) -> Result<KOwner, KeyError> {
        let mut k = [0u8; KOwner::LEN];
        getrandom::fill(&mut k).map_err(|e| KeyError(e.to_string()))?;
        Ok(KOwner(k))
    }
}
