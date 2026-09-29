//! The mailbox rules. [`policy`] and [`model`] are pure; [`usecases`] runs them against a [`crate::storage::Repo`]
//! inside the caller's transaction, with the time passed in.

pub mod model;
pub mod policy;
