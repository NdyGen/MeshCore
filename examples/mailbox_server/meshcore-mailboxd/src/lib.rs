#![forbid(unsafe_code)]
#![cfg_attr(
    not(test),
    deny(
        clippy::unwrap_used,
        clippy::expect_used,
        clippy::panic,
        clippy::todo,
        clippy::unreachable
    )
)]
//! `meshcore-mailboxd`: the message store of the MeshCore DM mailbox, next to the mailbox radio (`examples/mailbox_server`).
//!
//! Functional core, imperative shell:
//! - [`protocol`]: the `@MBX` / `mbx.` line protocol, parse and format only.
//! - [`domain`]: the mailbox rules (quota, TTL, states, reports) over a [`storage::Repo`], time as an argument.
//! - [`storage`]: the transaction boundary and its SQLite implementation.
//! - [`session`]: one radio session: lines in, replies out, a reply only after its commit.
//! - [`system`], [`transport`], [`cli`]: clock, randomness, stdio and serial port, command line.

pub mod cli;
pub mod domain;
pub mod mailbox;
pub mod protocol;
pub mod session;
pub mod storage;
pub mod system;
pub mod transport;
pub mod types;
