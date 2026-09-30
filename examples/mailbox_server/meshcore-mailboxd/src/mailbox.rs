//! The mailbox as a service: every operation is one transaction, and returns only after its commit.

use crate::domain::model::{Limits, Owner};
use crate::domain::policy::{AclEntry, StatusItem};
use crate::domain::usecases::{
    self, Deposit, FetchOutcome, FetchRequest, KeySource, RegOutcome, StoreOutcome,
};
use crate::storage::{Repo, Storage, StorageError, Transaction};
use crate::system::OsKeys;
use crate::types::{KOwner, Owner4, PktHash, Pubkey, Token, UnixTime};

pub use crate::domain::usecases::AdminError;

pub struct Mailbox<S: Storage> {
    storage: S,
    keys: Box<dyn KeySource>,
}

impl<S: Storage> Mailbox<S> {
    pub fn new(storage: S) -> Self {
        Self::with_keys(storage, Box::new(OsKeys))
    }

    pub fn with_keys(storage: S, keys: Box<dyn KeySource>) -> Self {
        Mailbox { storage, keys }
    }

    pub fn storage(&self) -> &S {
        &self.storage
    }

    fn atomically<T, E: From<StorageError>>(
        &mut self,
        f: impl FnOnce(&mut S::Tx<'_>, &mut dyn KeySource) -> Result<T, E>,
    ) -> Result<T, E> {
        let mut tx = self.storage.begin()?;
        let out = f(&mut tx, self.keys.as_mut())?;
        tx.commit()?;
        Ok(out)
    }

    pub fn store(&mut self, now: UnixTime, d: &Deposit<'_>) -> Result<StoreOutcome, StorageError> {
        self.atomically(|tx, _| usecases::store(tx, now, d))
    }

    pub fn register(
        &mut self,
        now: UnixTime,
        sender: &Pubkey,
        owner4: &Owner4,
        token: &Token,
    ) -> Result<RegOutcome, StorageError> {
        self.atomically(|tx, _| usecases::register(tx, now, sender, owner4, token))
    }

    pub fn fetch(
        &mut self,
        now: UnixTime,
        req: &FetchRequest<'_>,
    ) -> Result<FetchOutcome, StorageError> {
        self.atomically(|tx, _| usecases::fetch(tx, now, req))
    }

    pub fn status(
        &mut self,
        now: UnixTime,
        sender: &Pubkey,
        hashes: &[PktHash],
    ) -> Result<Vec<StatusItem>, StorageError> {
        self.atomically(|tx, _| usecases::status(tx, now, sender, hashes))
    }

    pub fn acl(&mut self) -> Result<Vec<AclEntry>, StorageError> {
        self.atomically(|tx, _| usecases::acl(tx))
    }

    /// Runs the timers without a request (the `advance` step of the conformance vectors).
    pub fn advance(&mut self, now: UnixTime) -> Result<(), StorageError> {
        self.atomically(|tx, _| usecases::housekeeping(tx, now))
    }

    pub fn owner_add(
        &mut self,
        now: UnixTime,
        pubkey: &Pubkey,
        k_owner: Option<KOwner>,
        limits: Limits,
    ) -> Result<KOwner, AdminError> {
        self.atomically(|tx, keys| usecases::owner_add(tx, now, pubkey, k_owner, &limits, keys))
    }

    pub fn owners(&mut self) -> Result<Vec<Owner>, StorageError> {
        self.atomically(|tx, _| tx.owners())
    }

    pub fn deny(
        &mut self,
        now: UnixTime,
        owner4: &Owner4,
        depositor: &Pubkey,
    ) -> Result<(), AdminError> {
        self.atomically(|tx, _| usecases::deny(tx, now, owner4, depositor))
    }

    pub fn undeny(&mut self, owner4: &Owner4, depositor: &Pubkey) -> Result<(), StorageError> {
        self.atomically(|tx, _| tx.undeny(owner4, depositor))
    }
}
