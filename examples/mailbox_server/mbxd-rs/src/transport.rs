//! Feeding a [`Session`] from a byte stream and writing its replies back: stdin/stdout or the serial port.

use std::collections::VecDeque;
use std::io::{self, BufRead, Read, Write};
use std::time::Duration;

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

/// Longest partial line kept from the serial port; more without a newline is noise.
pub const LINE_MAX: usize = 4096;

/// Splits a byte stream into lines. Invalid UTF-8 is replaced, so it reads as debug output.
#[derive(Debug, Default)]
pub struct LineAssembler {
    buf: Vec<u8>,
}

impl LineAssembler {
    pub fn push(&mut self, bytes: &[u8]) -> Vec<String> {
        self.buf.extend_from_slice(bytes);
        let mut lines = Vec::new();
        while let Some(nl) = self.buf.iter().position(|&b| b == b'\n') {
            let rest = self.buf.split_off(nl + 1);
            lines.push(String::from_utf8_lossy(&self.buf[..nl]).into_owned());
            self.buf = rest;
        }
        if self.buf.len() > LINE_MAX {
            log::warn!("dropping {} bytes without newline", self.buf.len());
            self.buf.clear();
        }
        lines
    }
}

/// Lines from stdin until EOF (the simulator's `SubprocessBackend`).
pub fn stdin_events(input: impl BufRead) -> impl Iterator<Item = io::Result<Event>> {
    input
        .split(b'\n')
        .map(|line| line.map(|l| Event::Line(String::from_utf8_lossy(&l).into_owned())))
}

/// Lines from the serial port, and [`Event::Idle`] after [`SERIAL_IDLE`] without a byte. Never ends by itself.
pub struct SerialEvents<R: Read> {
    port: R,
    lines: LineAssembler,
    pending: VecDeque<String>,
}

/// How long the port stays quiet before the session gets an [`Event::Idle`].
pub const SERIAL_IDLE: Duration = Duration::from_secs(1);

impl<R: Read> SerialEvents<R> {
    /// `port` must time out after [`SERIAL_IDLE`].
    pub fn new(port: R) -> Self {
        SerialEvents {
            port,
            lines: LineAssembler::default(),
            pending: VecDeque::new(),
        }
    }
}

impl<R: Read> Iterator for SerialEvents<R> {
    type Item = io::Result<Event>;

    fn next(&mut self) -> Option<Self::Item> {
        let mut buf = [0u8; 512];
        while self.pending.is_empty() {
            match self.port.read(&mut buf) {
                Ok(0) => return Some(Ok(Event::Idle)),
                Ok(n) => self.pending.extend(self.lines.push(&buf[..n])),
                Err(e) if e.kind() == io::ErrorKind::TimedOut => return Some(Ok(Event::Idle)),
                Err(e) if e.kind() == io::ErrorKind::Interrupted => {}
                Err(e) => return Some(Err(e)),
            }
        }
        self.pending.pop_front().map(|l| Ok(Event::Line(l)))
    }
}

/// Opens the radio's port once and keeps it: every open resets the ESP32, which then says HELLO.
pub fn open_serial(path: &str) -> Result<Box<dyn serialport::SerialPort>, serialport::Error> {
    let mut port = serialport::new(path, 115_200).timeout(SERIAL_IDLE).open()?;
    // DTR and RTS drive the ESP32's reset and boot pins; low keeps it running normally
    port.write_data_terminal_ready(false)?;
    port.write_request_to_send(false)?;
    Ok(port)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn assembler_splits_lines_across_reads() {
        let mut a = LineAssembler::default();
        assert!(a.push(b"@MBX HEL").is_empty());
        assert_eq!(a.push(b"LO\r\nnoise\n@MBX"), ["@MBX HELLO\r", "noise"]);
        assert_eq!(a.push(b" STAT\n\n"), ["@MBX STAT", ""]);
    }

    #[test]
    fn assembler_replaces_invalid_utf8() {
        let mut a = LineAssembler::default();
        assert_eq!(a.push(b"\xff@MBX\n"), ["\u{fffd}@MBX"]);
    }

    #[test]
    fn assembler_drops_an_endless_line() {
        let mut a = LineAssembler::default();
        assert!(a.push(&[b'x'; LINE_MAX]).is_empty());
        assert!(a.push(b"y").is_empty());
        assert_eq!(a.push(b"z\n"), ["z"], "the dropped bytes are gone");
    }

    /// Reads scripted chunks; `None` is a read timeout.
    struct Script(VecDeque<Option<&'static [u8]>>);

    impl Read for Script {
        fn read(&mut self, buf: &mut [u8]) -> io::Result<usize> {
            match self.0.pop_front() {
                Some(Some(chunk)) => {
                    buf[..chunk.len()].copy_from_slice(chunk);
                    Ok(chunk.len())
                }
                Some(None) => Err(io::ErrorKind::TimedOut.into()),
                None => Err(io::ErrorKind::BrokenPipe.into()),
            }
        }
    }

    #[test]
    fn serial_events_are_lines_and_idle_on_timeout() {
        let script = Script(VecDeque::from([
            Some(&b"a\nb"[..]),
            None,
            Some(b"\nc\nd\n"),
            None,
        ]));
        let events: Vec<_> = SerialEvents::new(script)
            .take(6)
            .map(|e| e.map_err(|e| e.kind()))
            .collect();
        let line = |s: &str| Ok(Event::Line(s.into()));
        assert_eq!(
            events,
            [
                line("a"),
                Ok(Event::Idle),
                line("b"),
                line("c"),
                line("d"),
                Ok(Event::Idle)
            ]
        );
    }

    #[test]
    fn serial_errors_end_the_session() {
        let mut events = SerialEvents::new(Script(VecDeque::new()));
        assert!(matches!(events.next(), Some(Err(e)) if e.kind() == io::ErrorKind::BrokenPipe));
    }
}
