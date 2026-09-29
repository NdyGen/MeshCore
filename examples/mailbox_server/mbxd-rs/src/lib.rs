#![forbid(unsafe_code)]
//! `mbxd`: the message store of the MeshCore DM mailbox, next to the mailbox radio (`examples/mailbox_server`).
//!
//! Functional core, imperative shell:
//! - [`protocol`]: the `@MBX` / `mbx.` line protocol, parse and format only.
//! - [`domain`]: the mailbox rules (quota, TTL, states, reports) over a [`storage::Repo`], time as an argument.
//! - [`storage`]: the transaction boundary and its SQLite implementation (schema shared with `mbxd.py`).
//! - [`session`]: one radio session: lines in, replies out, a reply only after its commit.
//! - [`system`], [`transport`], [`cli`]: clock, randomness, stdio and serial port, command line.

pub mod protocol;
pub mod types;
