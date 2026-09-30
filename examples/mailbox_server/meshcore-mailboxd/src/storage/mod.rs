//! The transaction boundary. A use case runs against a [`Repo`] inside one [`Transaction`]; the caller replies
//! only after [`Transaction::commit`] returned, and dropping a transaction rolls it back.

mod sqlite;

pub use sqlite::{SCHEMA, SqliteStorage};

use crate::domain::model::{Limits, Message, Owner};
use crate::domain::policy::Expiry;
use crate::types::{KOwner, Owner4, PktHash, Pubkey, State, UnixTime};

#[derive(Debug, thiserror::Error)]
pub enum StorageError {
    #[error("database: {0}")]
    Database(#[source] Box<dyn std::error::Error + Send + Sync>),
    #[error("unexpected value in the database: {0}")]
    Corrupt(String),
}

pub trait Storage {
    type Tx<'a>: Transaction
    where
        Self: 'a;

    /// Starts a write transaction; it holds the write lock until commit or drop.
    fn begin(&mut self) -> Result<Self::Tx<'_>, StorageError>;

    /// Whether another connection (the admin command line in its own process) committed since the previous
    /// call, or since open. The caller's own commits do not count.
    fn changed_elsewhere(&mut self) -> Result<bool, StorageError>;
}

pub trait Transaction: Repo {
    /// Makes every change durable, or none of them.
    fn commit(self) -> Result<(), StorageError>;
}

/// The mailbox's records. Queries return what is stored; the rules about them live in [`crate::domain`].
pub trait Repo {
    fn owner_by_prefix(&self, owner4: &Owner4) -> Result<Option<Owner>, StorageError>;
    fn owner_by_pubkey(&self, pubkey: &Pubkey) -> Result<Option<Owner>, StorageError>;
    /// In the order they were added.
    fn owners(&self) -> Result<Vec<Owner>, StorageError>;
    /// Adds the owner, or changes key and limits of an existing one (keeping `store_id` and the rotation).
    fn put_owner(
        &mut self,
        pubkey: &Pubkey,
        k_owner: &KOwner,
        limits: &Limits,
        now: UnixTime,
    ) -> Result<(), StorageError>;
    fn set_store_id(&mut self, owner4: &Owner4, store_id: u32) -> Result<(), StorageError>;
    fn set_last_sender(&mut self, owner4: &Owner4, sender: &Pubkey) -> Result<(), StorageError>;

    fn is_registered(&self, owner4: &Owner4, depositor: &Pubkey) -> Result<bool, StorageError>;
    /// Registers the depositor, or marks an existing registration as seen at `now`.
    fn register(
        &mut self,
        owner4: &Owner4,
        depositor: &Pubkey,
        now: UnixTime,
    ) -> Result<(), StorageError>;
    /// Counts a deposit and marks the registration as seen at `now`.
    fn count_deposit(
        &mut self,
        owner4: &Owner4,
        depositor: &Pubkey,
        now: UnixTime,
    ) -> Result<(), StorageError>;
    fn unregister(&mut self, owner4: &Owner4, depositor: &Pubkey) -> Result<(), StorageError>;
    /// Registrations with a known owner and not denylisted, most recently seen first.
    fn depositors_by_recency(&self) -> Result<Vec<(Pubkey, Owner4)>, StorageError>;

    fn is_denied(&self, owner4: &Owner4, depositor: &Pubkey) -> Result<bool, StorageError>;
    fn deny(
        &mut self,
        owner4: &Owner4,
        depositor: &Pubkey,
        now: UnixTime,
    ) -> Result<(), StorageError>;
    fn undeny(&mut self, owner4: &Owner4, depositor: &Pubkey) -> Result<(), StorageError>;

    fn message(&self, pkt_hash: &PktHash) -> Result<Option<Message>, StorageError>;
    fn insert_message(&mut self, m: &Message) -> Result<(), StorageError>;
    /// Writes the mutable fields (state, payload, deadlines, acks, `final_at`) of the message with this hash.
    fn update_message(&mut self, m: &Message) -> Result<(), StorageError>;
    /// Messages for `owner4` in one of `states`, of `sender` if given.
    fn count_messages(
        &self,
        owner4: &Owner4,
        sender: Option<&Pubkey>,
        states: &[State],
    ) -> Result<u32, StorageError>;
    fn messages(&self, owner4: &Owner4, states: &[State]) -> Result<Vec<Message>, StorageError>;
    /// Distinct senders with a message in `state` for `owner4`, ascending bytewise.
    fn senders(&self, owner4: &Owner4, state: State) -> Result<Vec<Pubkey>, StorageError>;
    /// The sender's earliest message in `state` for `owner4` (deposit time, then arrival).
    fn oldest(
        &self,
        owner4: &Owner4,
        sender: &Pubkey,
        state: State,
    ) -> Result<Option<Message>, StorageError>;
    /// Applies a timer to every message it has run out for; returns how many.
    fn expire(&mut self, rule: &Expiry, now: UnixTime) -> Result<usize, StorageError>;
    /// Removes messages that became final at or before `until`.
    fn purge_final(&mut self, until: UnixTime) -> Result<usize, StorageError>;
}
