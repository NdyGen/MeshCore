//! Feeding a [`Session`] from a byte stream and writing its replies back: stdin/stdout or the serial port.

use std::io::{self, Write};

use crate::session::Session;
use crate::storage::{Storage, StorageError};
use crate::system::Clock;

/// What the session reacts to.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Event {
    Line(String),
    /// Nothing arrived for a while; periodic lines may be due.
    Idle,
}

#[derive(Debug, thiserror::Error)]
pub enum ServeError {
    #[error("i/o: {0}")]
    Io(#[from] io::Error),
    #[error(transparent)]
    Storage(#[from] StorageError),
}

/// Runs the session until the events end, flushing the replies of every event before taking the next.
pub fn serve<S: Storage, C: Clock>(
    session: &mut Session<S, C>,
    events: impl IntoIterator<Item = io::Result<Event>>,
    out: &mut impl Write,
) -> Result<(), ServeError> {
    for event in events {
        let mut replies = match event? {
            Event::Line(line) => session.handle_line(&line)?,
            Event::Idle => Vec::new(),
        };
        replies.extend(session.tick());
        for reply in &replies {
            writeln!(out, "{reply}")?;
        }
        out.flush()?;
    }
    Ok(())
}
