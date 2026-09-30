//! One function per request or admin command, run inside the caller's transaction.

use super::model::{Limits, Message};
use super::policy::{self, AclEntry, StatusItem};
use crate::storage::{Repo, StorageError};
use crate::types::{
    Code, KOwner, Owner4, PktHash, Pubkey, Report, ReportResult, State, Token, UnixTime,
};

/// Where new `K_owner` secrets come from.
pub trait KeySource {
    fn k_owner(&mut self) -> Result<KOwner, KeyError>;
}

#[derive(Debug, thiserror::Error)]
#[error("no randomness for K_owner: {0}")]
pub struct KeyError(pub String);

#[derive(Debug, thiserror::Error)]
pub enum AdminError {
    #[error("another owner has prefix {0}")]
    PrefixTaken(Owner4),
    #[error("unknown owner {0}")]
    UnknownOwner(Owner4),
    #[error(transparent)]
    Key(#[from] KeyError),
    #[error(transparent)]
    Storage(#[from] StorageError),
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Deposit<'a> {
    pub owner4: Owner4,
    pub sender: Pubkey,
    pub pkt_hash: PktHash,
    pub payload: &'a [u8],
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct StoreOutcome {
    pub code: Code,
    /// T_radio of the stored copy; 0 unless OK or ALREADY_STORED.
    pub expires: UnixTime,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct RegOutcome {
    pub code: Code,
    pub ttl_days: u8,
    pub quota: u8,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct FetchRequest<'a> {
    pub client: Pubkey,
    /// The owner's inbox is full: reports only, no copy.
    pub no_payload: bool,
    pub store_id: u32,
    pub reports: &'a [Report],
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct FetchOutcome {
    pub code: Code,
    /// STORED copies left after this reply, capped at 255.
    pub remaining: u8,
    pub reports_ok: u8,
    pub payload: Option<Vec<u8>>,
    pub resync: Option<Resync>,
    pub ignored: Vec<IgnoredReport>,
}

/// The owner's radio came back with another `store_id`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Resync {
    pub owner4: Owner4,
    pub from: u32,
    pub to: u32,
    pub requeued: usize,
}

/// A confirmed report that changed nothing.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct IgnoredReport {
    pub pkt_hash: PktHash,
    pub result: ReportResult,
    /// `None`: no message with this hash for this owner.
    pub state: Option<State>,
}

/// The timers, then removal of rows 30 days after their final state. Runs at the start of every request.
pub fn housekeeping<R: Repo + ?Sized>(repo: &mut R, now: UnixTime) -> Result<(), StorageError> {
    for rule in &policy::EXPIRIES {
        repo.expire(rule, now)?;
    }
    repo.purge_final(policy::purge_until(now))?;
    Ok(())
}

pub fn store<R: Repo + ?Sized>(
    repo: &mut R,
    now: UnixTime,
    d: &Deposit<'_>,
) -> Result<StoreOutcome, StorageError> {
    let refuse = |code| Ok(StoreOutcome { code, expires: 0 });
    housekeeping(repo, now)?;
    if d.payload.len() > policy::MAX_INNER {
        return refuse(Code::TooBig);
    }
    let Some(owner) = repo.owner_by_prefix(&d.owner4)? else {
        return refuse(Code::UnknownOwner);
    };
    if repo.is_denied(&d.owner4, &d.sender)? || !repo.is_registered(&d.owner4, &d.sender)? {
        return refuse(Code::NotAuth);
    }
    if let Some(m) = repo.message(&d.pkt_hash)? {
        // a lost DEPOSIT reply: the same sender gets its original deadline, anyone else learns nothing
        if m.owner4 == d.owner4 && m.sender == d.sender {
            return Ok(StoreOutcome {
                code: Code::AlreadyStored,
                expires: m.t_radio,
            });
        }
        return refuse(Code::NotAuth);
    }
    if repo.count_messages(&d.owner4, Some(&d.sender), &policy::LIVE)?
        >= u32::from(owner.limits.quota())
    {
        return refuse(Code::Quota);
    }
    let m = Message::new(
        d.pkt_hash,
        d.owner4,
        d.sender,
        d.payload.to_vec(),
        now,
        &owner.limits,
    );
    repo.insert_message(&m)?;
    repo.count_deposit(&d.owner4, &d.sender, now)?;
    Ok(StoreOutcome {
        code: Code::Ok,
        expires: m.t_radio,
    })
}

pub fn register<R: Repo + ?Sized>(
    repo: &mut R,
    now: UnixTime,
    sender: &Pubkey,
    owner4: &Owner4,
    token: &Token,
) -> Result<RegOutcome, StorageError> {
    let refuse = |code| {
        Ok(RegOutcome {
            code,
            ttl_days: 0,
            quota: 0,
        })
    };
    housekeeping(repo, now)?;
    let Some(owner) = repo.owner_by_prefix(owner4)? else {
        return refuse(Code::UnknownOwner);
    };
    if repo.is_denied(owner4, sender)? || !policy::token_valid(&owner.k_owner, sender, token) {
        return refuse(Code::NotAuth);
    }
    repo.register(owner4, sender, now)?;
    Ok(RegOutcome {
        code: Code::Ok,
        ttl_days: owner.limits.ttl_days(),
        quota: owner.limits.quota(),
    })
}

pub fn fetch<R: Repo + ?Sized>(
    repo: &mut R,
    now: UnixTime,
    req: &FetchRequest<'_>,
) -> Result<FetchOutcome, StorageError> {
    housekeeping(repo, now)?;
    let Some(owner) = repo.owner_by_pubkey(&req.client)? else {
        return Ok(FetchOutcome {
            code: Code::NotAuth,
            remaining: 0,
            reports_ok: 0,
            payload: None,
            resync: None,
            ignored: vec![],
        });
    };
    let owner4 = owner.owner4();

    let resync = match owner.store_id {
        Some(from) if from != req.store_id => {
            let lost = repo.messages(&owner4, &policy::RESYNC_FROM)?;
            for m in &lost {
                repo.update_message(&policy::resynced(m, now, &owner.limits))?;
            }
            Some(Resync {
                owner4,
                from,
                to: req.store_id,
                requeued: lost.len(),
            })
        }
        _ => None,
    };
    repo.set_store_id(&owner4, req.store_id)?;

    let mut ignored = Vec::new();
    for report in req.reports {
        let m = repo
            .message(&report.pkt_hash)?
            .filter(|m| m.owner4 == owner4);
        match m
            .as_ref()
            .and_then(|m| policy::after_report(m, report, now, &owner.limits))
        {
            Some(next) => repo.update_message(&next)?,
            None => ignored.push(IgnoredReport {
                pkt_hash: report.pkt_hash,
                result: report.result,
                state: m.map(|m| m.state),
            }),
        }
    }

    // a copy the owner did not report on never reached her radio
    for m in repo.messages(&owner4, &[State::Sent])? {
        repo.update_message(&Message {
            state: State::Stored,
            ..m
        })?;
    }

    let mut payload = None;
    if !req.no_payload {
        let senders = repo.senders(&owner4, State::Stored)?;
        if let Some(sender) = policy::next_sender(&senders, owner.last_sender.as_ref())
            && let Some(m) = repo.oldest(&owner4, &sender, State::Stored)?
        {
            repo.update_message(&Message {
                state: State::Sent,
                ..m.clone()
            })?;
            repo.set_last_sender(&owner4, &sender)?;
            payload = m.payload;
        }
    }

    let remaining = repo.count_messages(&owner4, None, &[State::Stored])?;
    Ok(FetchOutcome {
        code: Code::Ok,
        remaining: u8::try_from(remaining).unwrap_or(u8::MAX),
        reports_ok: u8::try_from(req.reports.len()).unwrap_or(u8::MAX),
        payload,
        resync,
        ignored,
    })
}

pub fn status<R: Repo + ?Sized>(
    repo: &mut R,
    now: UnixTime,
    sender: &Pubkey,
    hashes: &[PktHash],
) -> Result<Vec<StatusItem>, StorageError> {
    housekeeping(repo, now)?;
    hashes
        .iter()
        .map(|h| Ok(policy::status_item(repo.message(h)?.as_ref(), sender)))
        .collect()
}

pub fn acl<R: Repo + ?Sized>(repo: &R) -> Result<Vec<AclEntry>, StorageError> {
    Ok(policy::acl(&repo.owners()?, &repo.depositors_by_recency()?))
}

/// Adds an owner or changes its limits; returns `K_owner`, which is kept unless a new one is given.
pub fn owner_add<R: Repo + ?Sized>(
    repo: &mut R,
    now: UnixTime,
    pubkey: &Pubkey,
    k_owner: Option<KOwner>,
    limits: &Limits,
    keys: &mut dyn KeySource,
) -> Result<KOwner, AdminError> {
    // owners are named by their prefix on the wire, so a prefix identifies one owner
    let existing = repo.owner_by_prefix(&pubkey.owner4())?;
    if existing.as_ref().is_some_and(|o| o.pubkey != *pubkey) {
        return Err(AdminError::PrefixTaken(pubkey.owner4()));
    }
    let k = match (k_owner, existing) {
        (Some(k), _) => k,
        (None, Some(o)) => o.k_owner,
        (None, None) => keys.k_owner()?,
    };
    repo.put_owner(pubkey, &k, limits, now)?;
    Ok(k)
}

/// Denylists a depositor for one owner and drops its registration.
pub fn deny<R: Repo + ?Sized>(
    repo: &mut R,
    now: UnixTime,
    owner4: &Owner4,
    depositor: &Pubkey,
) -> Result<(), AdminError> {
    if repo.owner_by_prefix(owner4)?.is_none() {
        return Err(AdminError::UnknownOwner(*owner4));
    }
    repo.deny(owner4, depositor, now)?;
    repo.unregister(owner4, depositor)?;
    Ok(())
}
