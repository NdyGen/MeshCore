//! [`Storage`] on SQLite, with the schema and pragmas of `mbxd.py`, so either daemon opens the other's database.

use std::path::Path;
use std::time::Duration;

use rusqlite::types::Value;
use rusqlite::{Connection, OptionalExtension, Row, TransactionBehavior, params, params_from_iter};

use super::{Repo, Storage, StorageError, Transaction};
use crate::domain::model::{Limits, Message, Owner};
use crate::domain::policy::{Deadline, Expiry};
use crate::types::{Ack, KOwner, Owner4, PktHash, Pubkey, State, UnixTime};

/// Verbatim from `mbxd.py`.
pub const SCHEMA: &str = "
CREATE TABLE IF NOT EXISTS owners (
    pubkey BLOB PRIMARY KEY, owner4 BLOB NOT NULL UNIQUE, k_owner BLOB NOT NULL,
    ttl_days INTEGER NOT NULL, sync_days INTEGER NOT NULL, quota INTEGER NOT NULL,
    store_id INTEGER, created INTEGER NOT NULL, last_sender BLOB);
CREATE TABLE IF NOT EXISTS acl (
    pubkey BLOB NOT NULL, owner4 BLOB NOT NULL, sent_count INTEGER NOT NULL DEFAULT 0,
    created INTEGER NOT NULL, last_seen INTEGER NOT NULL, PRIMARY KEY (pubkey, owner4));
CREATE TABLE IF NOT EXISTS denylist (
    owner4 BLOB NOT NULL, pubkey BLOB NOT NULL, created INTEGER NOT NULL, PRIMARY KEY (owner4, pubkey));
CREATE TABLE IF NOT EXISTS messages (
    pkt_hash BLOB PRIMARY KEY, owner4 BLOB NOT NULL, sender BLOB NOT NULL, payload BLOB,
    created INTEGER NOT NULL, t_radio INTEGER NOT NULL, t_sync INTEGER, state TEXT NOT NULL,
    ack_r BLOB, ack_s BLOB, final_at INTEGER);
CREATE INDEX IF NOT EXISTS messages_owner_state ON messages (owner4, state, created);
";

/// Python's `sqlite3.connect` default: a second process (the admin CLI next to `serve`) waits for the lock.
const BUSY_TIMEOUT: Duration = Duration::from_secs(5);

impl From<rusqlite::Error> for StorageError {
    fn from(e: rusqlite::Error) -> Self {
        StorageError::Database(Box::new(e))
    }
}

pub struct SqliteStorage {
    conn: Connection,
}

impl SqliteStorage {
    /// Opens or creates the database: WAL, `synchronous=FULL`, and the schema, adding the rotation column to a
    /// database from before it existed.
    pub fn open(path: impl AsRef<Path>) -> Result<Self, StorageError> {
        let conn = Connection::open(path)?;
        conn.busy_timeout(BUSY_TIMEOUT)?;
        conn.query_row("PRAGMA journal_mode=WAL", [], |_| Ok(()))?;
        conn.pragma_update(None, "synchronous", "FULL")?;
        conn.execute_batch(SCHEMA)?;
        let has_rotation = conn
            .prepare("SELECT 1 FROM pragma_table_info('owners') WHERE name = 'last_sender'")?
            .exists([])?;
        if !has_rotation {
            conn.execute("ALTER TABLE owners ADD COLUMN last_sender BLOB", [])?;
        }
        Ok(SqliteStorage { conn })
    }

    /// For tests and diagnostics; changing the database behind the storage's back is on the caller.
    pub fn connection(&self) -> &Connection {
        &self.conn
    }
}

impl Storage for SqliteStorage {
    type Tx<'a> = SqliteTx<'a>;

    fn begin(&mut self) -> Result<SqliteTx<'_>, StorageError> {
        Ok(SqliteTx {
            tx: self
                .conn
                .transaction_with_behavior(TransactionBehavior::Immediate)?,
        })
    }
}

pub struct SqliteTx<'a> {
    tx: rusqlite::Transaction<'a>,
}

impl Transaction for SqliteTx<'_> {
    fn commit(self) -> Result<(), StorageError> {
        Ok(self.tx.commit()?)
    }
}

fn corrupt(what: &str) -> StorageError {
    StorageError::Corrupt(what.to_string())
}

fn fixed<const N: usize>(bytes: Vec<u8>, what: &str) -> Result<[u8; N], StorageError> {
    bytes.try_into().map_err(|_| corrupt(what))
}

fn state_from(name: String) -> Result<State, StorageError> {
    State::from_db_name(&name)
        .ok_or_else(|| StorageError::Corrupt(format!("message state {name:?}")))
}

/// The row with SQL types, converted outside the rusqlite callback so conversion errors stay [`StorageError`].
struct OwnerRow(
    Vec<u8>,
    Vec<u8>,
    i64,
    i64,
    i64,
    Option<i64>,
    Option<Vec<u8>>,
);

const OWNER_COLUMNS: &str = "pubkey, k_owner, ttl_days, sync_days, quota, store_id, last_sender";

fn owner_row(r: &Row<'_>) -> rusqlite::Result<OwnerRow> {
    Ok(OwnerRow(
        r.get(0)?,
        r.get(1)?,
        r.get(2)?,
        r.get(3)?,
        r.get(4)?,
        r.get(5)?,
        r.get(6)?,
    ))
}

impl TryFrom<OwnerRow> for Owner {
    type Error = StorageError;

    fn try_from(
        OwnerRow(pubkey, k_owner, ttl, sync, quota, store_id, last_sender): OwnerRow,
    ) -> Result<Owner, StorageError> {
        Ok(Owner {
            pubkey: Pubkey(fixed(pubkey, "owner pubkey")?),
            k_owner: KOwner(fixed(k_owner, "k_owner")?),
            limits: Limits::new(ttl, sync, quota).map_err(|_| corrupt("owner limits"))?,
            store_id: store_id
                .map(|s| u32::try_from(s).map_err(|_| corrupt("store_id")))
                .transpose()?,
            last_sender: last_sender
                .map(|s| fixed(s, "last_sender").map(Pubkey))
                .transpose()?,
        })
    }
}

struct MessageRow {
    pkt_hash: Vec<u8>,
    owner4: Vec<u8>,
    sender: Vec<u8>,
    payload: Option<Vec<u8>>,
    created: i64,
    t_radio: i64,
    t_sync: Option<i64>,
    state: String,
    ack_r: Option<Vec<u8>>,
    ack_s: Option<Vec<u8>>,
    final_at: Option<i64>,
}

const MESSAGE_COLUMNS: &str =
    "pkt_hash, owner4, sender, payload, created, t_radio, t_sync, state, ack_r, ack_s, final_at";

fn message_row(r: &Row<'_>) -> rusqlite::Result<MessageRow> {
    Ok(MessageRow {
        pkt_hash: r.get(0)?,
        owner4: r.get(1)?,
        sender: r.get(2)?,
        payload: r.get(3)?,
        created: r.get(4)?,
        t_radio: r.get(5)?,
        t_sync: r.get(6)?,
        state: r.get(7)?,
        ack_r: r.get(8)?,
        ack_s: r.get(9)?,
        final_at: r.get(10)?,
    })
}

impl TryFrom<MessageRow> for Message {
    type Error = StorageError;

    fn try_from(r: MessageRow) -> Result<Message, StorageError> {
        let ack = |a: Option<Vec<u8>>| a.map(|a| fixed(a, "ack").map(Ack)).transpose();
        Ok(Message {
            pkt_hash: PktHash(fixed(r.pkt_hash, "pkt_hash")?),
            owner4: Owner4(fixed(r.owner4, "message owner4")?),
            sender: Pubkey(fixed(r.sender, "sender")?),
            payload: r.payload,
            created: r.created,
            t_radio: r.t_radio,
            t_sync: r.t_sync,
            state: state_from(r.state)?,
            ack_r: ack(r.ack_r)?,
            ack_s: ack(r.ack_s)?,
            final_at: r.final_at,
        })
    }
}

fn convert<R, T>(rows: Vec<R>) -> Result<Vec<T>, StorageError>
where
    T: TryFrom<R, Error = StorageError>,
{
    rows.into_iter().map(T::try_from).collect()
}

fn placeholders(n: usize) -> String {
    vec!["?"; n].join(",")
}

fn blob(b: &[u8]) -> Value {
    Value::Blob(b.to_vec())
}

fn state_values(states: &[State]) -> impl Iterator<Item = Value> + '_ {
    states.iter().map(|s| Value::Text(s.db_name().to_string()))
}

impl SqliteTx<'_> {
    fn owner_where(&self, column: &str, key: &[u8]) -> Result<Option<Owner>, StorageError> {
        let sql = format!("SELECT {OWNER_COLUMNS} FROM owners WHERE {column}=?");
        let row = self
            .tx
            .prepare_cached(&sql)?
            .query_row([key], owner_row)
            .optional()?;
        row.map(Owner::try_from).transpose()
    }

    fn exists(&self, sql: &str, a: &[u8], b: &[u8]) -> Result<bool, StorageError> {
        Ok(self.tx.prepare_cached(sql)?.exists([a, b])?)
    }
}

impl Repo for SqliteTx<'_> {
    fn owner_by_prefix(&self, owner4: &Owner4) -> Result<Option<Owner>, StorageError> {
        self.owner_where("owner4", &owner4.0)
    }

    fn owner_by_pubkey(&self, pubkey: &Pubkey) -> Result<Option<Owner>, StorageError> {
        self.owner_where("pubkey", &pubkey.0)
    }

    fn owners(&self) -> Result<Vec<Owner>, StorageError> {
        let sql = format!("SELECT {OWNER_COLUMNS} FROM owners ORDER BY created, rowid");
        let rows = self
            .tx
            .prepare_cached(&sql)?
            .query_map([], owner_row)?
            .collect::<Result<Vec<_>, _>>()?;
        convert(rows)
    }

    fn put_owner(
        &mut self,
        pubkey: &Pubkey,
        k_owner: &KOwner,
        limits: &Limits,
        now: UnixTime,
    ) -> Result<(), StorageError> {
        self.tx
            .prepare_cached(
                "INSERT INTO owners (pubkey, owner4, k_owner, ttl_days, sync_days, quota, created) \
                 VALUES (?,?,?,?,?,?,?) ON CONFLICT (pubkey) DO UPDATE SET k_owner=excluded.k_owner, \
                 ttl_days=excluded.ttl_days, sync_days=excluded.sync_days, quota=excluded.quota",
            )?
            .execute(params![
                &pubkey.0[..],
                &pubkey.owner4().0[..],
                &k_owner.0[..],
                limits.ttl_days(),
                limits.sync_days(),
                limits.quota(),
                now
            ])?;
        Ok(())
    }

    fn set_store_id(&mut self, owner4: &Owner4, store_id: u32) -> Result<(), StorageError> {
        self.tx
            .prepare_cached("UPDATE owners SET store_id=? WHERE owner4=?")?
            .execute(params![store_id, &owner4.0[..]])?;
        Ok(())
    }

    fn set_last_sender(&mut self, owner4: &Owner4, sender: &Pubkey) -> Result<(), StorageError> {
        self.tx
            .prepare_cached("UPDATE owners SET last_sender=? WHERE owner4=?")?
            .execute(params![&sender.0[..], &owner4.0[..]])?;
        Ok(())
    }

    fn is_registered(&self, owner4: &Owner4, depositor: &Pubkey) -> Result<bool, StorageError> {
        self.exists(
            "SELECT 1 FROM acl WHERE pubkey=? AND owner4=?",
            &depositor.0,
            &owner4.0,
        )
    }

    fn register(
        &mut self,
        owner4: &Owner4,
        depositor: &Pubkey,
        now: UnixTime,
    ) -> Result<(), StorageError> {
        self.tx
            .prepare_cached(
                "INSERT INTO acl (pubkey, owner4, created, last_seen) VALUES (?,?,?,?) \
                 ON CONFLICT (pubkey, owner4) DO UPDATE SET last_seen=excluded.last_seen",
            )?
            .execute(params![&depositor.0[..], &owner4.0[..], now, now])?;
        Ok(())
    }

    fn count_deposit(
        &mut self,
        owner4: &Owner4,
        depositor: &Pubkey,
        now: UnixTime,
    ) -> Result<(), StorageError> {
        self.tx
            .prepare_cached(
                "UPDATE acl SET sent_count=sent_count+1, last_seen=? WHERE pubkey=? AND owner4=?",
            )?
            .execute(params![now, &depositor.0[..], &owner4.0[..]])?;
        Ok(())
    }

    fn unregister(&mut self, owner4: &Owner4, depositor: &Pubkey) -> Result<(), StorageError> {
        self.tx
            .prepare_cached("DELETE FROM acl WHERE owner4=? AND pubkey=?")?
            .execute(params![&owner4.0[..], &depositor.0[..]])?;
        Ok(())
    }

    fn depositors_by_recency(&self) -> Result<Vec<(Pubkey, Owner4)>, StorageError> {
        let rows = self
            .tx
            .prepare_cached(
                "SELECT a.pubkey, a.owner4 FROM acl a JOIN owners o ON o.owner4=a.owner4 \
                 WHERE NOT EXISTS (SELECT 1 FROM denylist d WHERE d.owner4=a.owner4 AND d.pubkey=a.pubkey) \
                 ORDER BY a.last_seen DESC, a.rowid DESC",
            )?
            .query_map([], |r| Ok((r.get::<_, Vec<u8>>(0)?, r.get::<_, Vec<u8>>(1)?)))?
            .collect::<Result<Vec<_>, _>>()?;
        rows.into_iter()
            .map(|(p, o)| {
                Ok((
                    Pubkey(fixed(p, "acl pubkey")?),
                    Owner4(fixed(o, "acl owner4")?),
                ))
            })
            .collect()
    }

    fn is_denied(&self, owner4: &Owner4, depositor: &Pubkey) -> Result<bool, StorageError> {
        self.exists(
            "SELECT 1 FROM denylist WHERE owner4=? AND pubkey=?",
            &owner4.0,
            &depositor.0,
        )
    }

    fn deny(
        &mut self,
        owner4: &Owner4,
        depositor: &Pubkey,
        now: UnixTime,
    ) -> Result<(), StorageError> {
        self.tx
            .prepare_cached(
                "INSERT OR IGNORE INTO denylist (owner4, pubkey, created) VALUES (?,?,?)",
            )?
            .execute(params![&owner4.0[..], &depositor.0[..], now])?;
        Ok(())
    }

    fn undeny(&mut self, owner4: &Owner4, depositor: &Pubkey) -> Result<(), StorageError> {
        self.tx
            .prepare_cached("DELETE FROM denylist WHERE owner4=? AND pubkey=?")?
            .execute(params![&owner4.0[..], &depositor.0[..]])?;
        Ok(())
    }

    fn message(&self, pkt_hash: &PktHash) -> Result<Option<Message>, StorageError> {
        let sql = format!("SELECT {MESSAGE_COLUMNS} FROM messages WHERE pkt_hash=?");
        let row = self
            .tx
            .prepare_cached(&sql)?
            .query_row([&pkt_hash.0[..]], message_row)
            .optional()?;
        row.map(Message::try_from).transpose()
    }

    fn insert_message(&mut self, m: &Message) -> Result<(), StorageError> {
        self.tx
            .prepare_cached(&format!(
                "INSERT INTO messages ({MESSAGE_COLUMNS}) VALUES (?,?,?,?,?,?,?,?,?,?,?)"
            ))?
            .execute(params![
                &m.pkt_hash.0[..],
                &m.owner4.0[..],
                &m.sender.0[..],
                m.payload,
                m.created,
                m.t_radio,
                m.t_sync,
                m.state.db_name(),
                m.ack_r.map(|a| a.0.to_vec()),
                m.ack_s.map(|a| a.0.to_vec()),
                m.final_at
            ])?;
        Ok(())
    }

    fn update_message(&mut self, m: &Message) -> Result<(), StorageError> {
        self.tx
            .prepare_cached(
                "UPDATE messages SET payload=?, t_radio=?, t_sync=?, state=?, ack_r=?, ack_s=?, final_at=? \
                 WHERE pkt_hash=?",
            )?
            .execute(params![
                m.payload,
                m.t_radio,
                m.t_sync,
                m.state.db_name(),
                m.ack_r.map(|a| a.0.to_vec()),
                m.ack_s.map(|a| a.0.to_vec()),
                m.final_at,
                &m.pkt_hash.0[..]
            ])?;
        Ok(())
    }

    fn count_messages(
        &self,
        owner4: &Owner4,
        sender: Option<&Pubkey>,
        states: &[State],
    ) -> Result<u32, StorageError> {
        let by_sender = if sender.is_some() {
            " AND sender=?"
        } else {
            ""
        };
        let sql = format!(
            "SELECT COUNT(*) FROM messages WHERE owner4=?{by_sender} AND state IN ({})",
            placeholders(states.len())
        );
        let args = std::iter::once(blob(&owner4.0))
            .chain(sender.map(|s| blob(&s.0)))
            .chain(state_values(states));
        let n: i64 = self
            .tx
            .prepare_cached(&sql)?
            .query_row(params_from_iter(args), |r| r.get(0))?;
        Ok(u32::try_from(n).unwrap_or(u32::MAX))
    }

    fn messages(&self, owner4: &Owner4, states: &[State]) -> Result<Vec<Message>, StorageError> {
        let sql = format!(
            "SELECT {MESSAGE_COLUMNS} FROM messages WHERE owner4=? AND state IN ({}) ORDER BY created, rowid",
            placeholders(states.len())
        );
        let args = std::iter::once(blob(&owner4.0)).chain(state_values(states));
        let rows = self
            .tx
            .prepare_cached(&sql)?
            .query_map(params_from_iter(args), message_row)?
            .collect::<Result<Vec<_>, _>>()?;
        convert(rows)
    }

    fn senders(&self, owner4: &Owner4, state: State) -> Result<Vec<Pubkey>, StorageError> {
        let rows = self
            .tx
            .prepare_cached(
                "SELECT DISTINCT sender FROM messages WHERE owner4=? AND state=? ORDER BY sender",
            )?
            .query_map(params![&owner4.0[..], state.db_name()], |r| {
                r.get::<_, Vec<u8>>(0)
            })?
            .collect::<Result<Vec<_>, _>>()?;
        rows.into_iter()
            .map(|s| fixed(s, "sender").map(Pubkey))
            .collect()
    }

    fn oldest(
        &self,
        owner4: &Owner4,
        sender: &Pubkey,
        state: State,
    ) -> Result<Option<Message>, StorageError> {
        let sql = format!(
            "SELECT {MESSAGE_COLUMNS} FROM messages WHERE owner4=? AND state=? AND sender=? ORDER BY created, rowid LIMIT 1"
        );
        let row = self
            .tx
            .prepare_cached(&sql)?
            .query_row(
                params![&owner4.0[..], state.db_name(), &sender.0[..]],
                message_row,
            )
            .optional()?;
        row.map(Message::try_from).transpose()
    }

    fn expire(&mut self, rule: &Expiry, now: UnixTime) -> Result<usize, StorageError> {
        let deadline = match rule.deadline {
            Deadline::TRadio => "t_radio",
            Deadline::TSync => "t_sync",
        };
        let sql = format!(
            "UPDATE messages SET state=?, payload=NULL, final_at={deadline} WHERE state IN ({}) AND {deadline} <= ?",
            placeholders(rule.from.len())
        );
        let args = std::iter::once(Value::Text(rule.to.db_name().into()))
            .chain(state_values(rule.from))
            .chain([Value::Integer(now)]);
        Ok(self
            .tx
            .prepare_cached(&sql)?
            .execute(params_from_iter(args))?)
    }

    fn purge_final(&mut self, until: UnixTime) -> Result<usize, StorageError> {
        Ok(self
            .tx
            .prepare_cached("DELETE FROM messages WHERE final_at IS NOT NULL AND final_at <= ?")?
            .execute([until])?)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::domain::model::DAY;
    use crate::domain::policy::EXPIRIES;

    const NOW: UnixTime = 1_790_000_000;

    fn open() -> (tempfile::TempDir, SqliteStorage) {
        let dir = tempfile::tempdir().unwrap();
        let s = SqliteStorage::open(dir.path().join("t.db")).unwrap();
        (dir, s)
    }

    fn pk(b: u8) -> Pubkey {
        Pubkey([b; 32])
    }

    fn msg(hash: u8, owner: &Pubkey, sender: &Pubkey, created: UnixTime) -> Message {
        Message::new(
            PktHash([hash; 8]),
            owner.owner4(),
            *sender,
            vec![hash; 10],
            created,
            &Limits::default(),
        )
    }

    #[test]
    fn pragmas_and_schema() {
        let (_d, s) = open();
        let c = s.connection();
        let sync: i64 = c.query_row("PRAGMA synchronous", [], |r| r.get(0)).unwrap();
        let journal: String = c
            .query_row("PRAGMA journal_mode", [], |r| r.get(0))
            .unwrap();
        assert_eq!((sync, journal.as_str()), (2, "wal"));
        let tables: Vec<String> = c
            .prepare("SELECT name FROM sqlite_master WHERE type='table' ORDER BY name")
            .unwrap()
            .query_map([], |r| r.get(0))
            .unwrap()
            .collect::<Result<_, _>>()
            .unwrap();
        assert_eq!(tables, ["acl", "denylist", "messages", "owners"]);
    }

    #[test]
    fn opens_a_database_without_the_rotation_column() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("old.db");
        Connection::open(&path)
            .unwrap()
            .execute_batch(&SCHEMA.replace(", last_sender BLOB", ""))
            .unwrap();
        let mut s = SqliteStorage::open(&path).unwrap();
        let mut tx = s.begin().unwrap();
        tx.put_owner(&pk(1), &KOwner([0; 16]), &Limits::default(), NOW)
            .unwrap();
        tx.set_last_sender(&pk(1).owner4(), &pk(2)).unwrap();
        assert_eq!(
            tx.owner_by_pubkey(&pk(1)).unwrap().unwrap().last_sender,
            Some(pk(2))
        );
    }

    #[test]
    fn owners_are_added_and_updated_in_place() {
        let (_d, mut s) = open();
        let mut tx = s.begin().unwrap();
        tx.put_owner(&pk(2), &KOwner([1; 16]), &Limits::default(), NOW)
            .unwrap();
        tx.put_owner(&pk(1), &KOwner([2; 16]), &Limits::default(), NOW + 1)
            .unwrap();
        tx.set_store_id(&pk(2).owner4(), 0xdead_beef).unwrap();
        tx.set_last_sender(&pk(2).owner4(), &pk(9)).unwrap();
        let limits = Limits::new(3, 4, 5).unwrap();
        tx.put_owner(&pk(2), &KOwner([7; 16]), &limits, NOW + 50)
            .unwrap();
        let o = tx.owner_by_prefix(&pk(2).owner4()).unwrap().unwrap();
        let want = Owner {
            pubkey: pk(2),
            k_owner: KOwner([7; 16]),
            limits,
            store_id: Some(0xdead_beef),
            last_sender: Some(pk(9)),
        };
        assert_eq!(o, want);
        assert_eq!(tx.owner_by_pubkey(&pk(2)).unwrap(), Some(want));
        assert_eq!(
            tx.owners()
                .unwrap()
                .iter()
                .map(|o| o.pubkey)
                .collect::<Vec<_>>(),
            [pk(2), pk(1)],
            "creation order"
        );
        assert_eq!(tx.owner_by_prefix(&Owner4([9; 4])).unwrap(), None);
    }

    #[test]
    fn transactions_commit_or_roll_back() {
        let (_d, mut s) = open();
        {
            let mut tx = s.begin().unwrap();
            tx.put_owner(&pk(1), &KOwner([0; 16]), &Limits::default(), NOW)
                .unwrap();
        }
        assert!(
            s.begin().unwrap().owners().unwrap().is_empty(),
            "dropped without commit"
        );
        let mut tx = s.begin().unwrap();
        tx.put_owner(&pk(1), &KOwner([0; 16]), &Limits::default(), NOW)
            .unwrap();
        tx.commit().unwrap();
        assert_eq!(s.begin().unwrap().owners().unwrap().len(), 1);
    }

    #[test]
    fn messages_roundtrip() {
        let (_d, mut s) = open();
        let mut tx = s.begin().unwrap();
        let mut m = msg(1, &pk(1), &pk(2), NOW);
        tx.insert_message(&m).unwrap();
        assert_eq!(tx.message(&m.pkt_hash).unwrap(), Some(m.clone()));
        m = Message {
            state: State::OnRadio,
            t_sync: Some(NOW + 5),
            ack_r: Some(Ack([1; 6])),
            ack_s: Some(Ack([2; 6])),
            ..m
        };
        tx.update_message(&m).unwrap();
        assert_eq!(tx.message(&m.pkt_hash).unwrap(), Some(m.clone()));
        m = Message {
            payload: None,
            final_at: Some(NOW + 9),
            state: State::Delivered,
            ..m
        };
        tx.update_message(&m).unwrap();
        assert_eq!(tx.message(&m.pkt_hash).unwrap(), Some(m));
        assert_eq!(tx.message(&PktHash([9; 8])).unwrap(), None);
    }

    #[test]
    fn message_queries() {
        let (_d, mut s) = open();
        let mut tx = s.begin().unwrap();
        let (o, other) = (pk(1), pk(2));
        let (a, b) = (pk(0x20), pk(0x10));
        tx.insert_message(&msg(1, &o, &a, NOW + 5)).unwrap();
        tx.insert_message(&msg(2, &o, &a, NOW)).unwrap();
        tx.insert_message(&msg(3, &o, &b, NOW)).unwrap();
        tx.insert_message(&msg(4, &o, &a, NOW)).unwrap();
        tx.insert_message(&msg(5, &other, &a, NOW)).unwrap();
        tx.update_message(&Message {
            state: State::Sent,
            ..msg(3, &o, &b, NOW)
        })
        .unwrap();

        let o4 = o.owner4();
        assert_eq!(tx.count_messages(&o4, None, &[State::Stored]).unwrap(), 3);
        assert_eq!(
            tx.count_messages(&o4, Some(&a), &[State::Stored, State::Sent])
                .unwrap(),
            3
        );
        assert_eq!(
            tx.count_messages(&o4, Some(&b), &[State::Stored, State::Sent])
                .unwrap(),
            1
        );
        let hashes = |ms: Vec<Message>| ms.iter().map(|m| m.pkt_hash.0[0]).collect::<Vec<_>>();
        assert_eq!(
            hashes(tx.messages(&o4, &[State::Stored, State::Sent]).unwrap()),
            [2, 3, 4, 1],
            "created, then arrival"
        );
        assert_eq!(tx.senders(&o4, State::Stored).unwrap(), [a]);
        assert_eq!(tx.senders(&other.owner4(), State::Stored).unwrap(), [a]);
        tx.update_message(&msg(3, &o, &b, NOW)).unwrap();
        assert_eq!(
            tx.senders(&o4, State::Stored).unwrap(),
            [b, a],
            "ascending bytewise"
        );
        assert_eq!(
            tx.oldest(&o4, &a, State::Stored)
                .unwrap()
                .map(|m| m.pkt_hash.0[0]),
            Some(2)
        );
        assert_eq!(tx.oldest(&o4, &a, State::OnRadio).unwrap(), None);
    }

    #[test]
    fn timers_and_purge() {
        let (_d, mut s) = open();
        let mut tx = s.begin().unwrap();
        let (o, a) = (pk(1), pk(2));
        let stored = msg(1, &o, &a, NOW);
        let on_radio = Message {
            state: State::OnRadio,
            t_sync: Some(NOW + DAY),
            ack_r: Some(Ack([3; 6])),
            ..msg(2, &o, &a, NOW)
        };
        for m in [&stored, &on_radio] {
            tx.insert_message(m).unwrap();
        }
        let [radio, sync] = EXPIRIES;
        assert_eq!(tx.expire(&sync, NOW + DAY - 1).unwrap(), 0);
        assert_eq!(tx.expire(&sync, NOW + DAY).unwrap(), 1);
        assert_eq!(tx.expire(&radio, NOW + 100 * DAY).unwrap(), 1);
        let e = tx.message(&stored.pkt_hash).unwrap().unwrap();
        assert_eq!(
            e,
            radio.apply(&stored, NOW + 100 * DAY).unwrap(),
            "same as the pure rule"
        );
        let se = tx.message(&on_radio.pkt_hash).unwrap().unwrap();
        assert_eq!(se, sync.apply(&on_radio, NOW + DAY).unwrap());
        assert_eq!(tx.purge_final(NOW + DAY - 1).unwrap(), 0);
        assert_eq!(tx.purge_final(NOW + DAY).unwrap(), 1);
        assert_eq!(tx.purge_final(stored.t_radio).unwrap(), 1);
        assert_eq!(
            tx.count_messages(&o.owner4(), None, &State::ALL).unwrap(),
            0
        );
    }

    #[test]
    fn registrations_and_denylist() {
        let (_d, mut s) = open();
        let mut tx = s.begin().unwrap();
        let (o1, o2, ghost) = (pk(1).owner4(), pk(2).owner4(), Owner4([9; 4]));
        tx.put_owner(&pk(1), &KOwner([0; 16]), &Limits::default(), NOW)
            .unwrap();
        tx.put_owner(&pk(2), &KOwner([0; 16]), &Limits::default(), NOW)
            .unwrap();
        tx.register(&o1, &pk(10), NOW).unwrap();
        tx.register(&o1, &pk(11), NOW + 1).unwrap();
        tx.register(&o2, &pk(12), NOW + 1).unwrap();
        tx.register(&ghost, &pk(13), NOW + 9).unwrap();
        tx.count_deposit(&o1, &pk(10), NOW + 2).unwrap();
        assert!(tx.is_registered(&o1, &pk(10)).unwrap());
        assert!(!tx.is_registered(&o2, &pk(10)).unwrap());
        let order = |tx: &SqliteTx<'_>| {
            tx.depositors_by_recency()
                .unwrap()
                .iter()
                .map(|(p, _)| p.0[0])
                .collect::<Vec<_>>()
        };
        assert_eq!(
            order(&tx),
            [10, 12, 11],
            "last seen, then latest registration; unknown owner left out"
        );

        tx.deny(&o1, &pk(11), NOW).unwrap();
        tx.deny(&o1, &pk(11), NOW).unwrap();
        assert!(tx.is_denied(&o1, &pk(11)).unwrap());
        assert!(!tx.is_denied(&o2, &pk(11)).unwrap());
        assert_eq!(order(&tx), [10, 12]);
        tx.undeny(&o1, &pk(11)).unwrap();
        assert_eq!(order(&tx), [10, 12, 11]);
        tx.unregister(&o1, &pk(11)).unwrap();
        assert_eq!(order(&tx), [10, 12]);
        let sent: i64 = tx
            .tx
            .query_row(
                "SELECT sent_count FROM acl WHERE pubkey=?",
                [&pk(10).0[..]],
                |r| r.get(0),
            )
            .unwrap();
        assert_eq!(sent, 1);
    }

    #[test]
    fn unexpected_values_are_storage_errors() {
        let (_d, mut s) = open();
        let tx = s.begin().unwrap();
        tx.tx
            .execute(
                "INSERT INTO messages (pkt_hash, owner4, sender, created, t_radio, state) VALUES (?, ?, ?, 0, 0, 'bogus')",
                params![&[1u8; 8][..], &[2u8; 4][..], &[3u8; 32][..]],
            )
            .unwrap();
        assert!(matches!(
            tx.message(&PktHash([1; 8])),
            Err(StorageError::Corrupt(_))
        ));
        tx.tx
            .execute(
                "INSERT INTO owners VALUES (?, ?, ?, 99, 30, 20, NULL, 0, NULL)",
                params![&[1u8; 32][..], &[1u8; 4][..], &[0u8; 16][..]],
            )
            .unwrap();
        assert!(matches!(tx.owners(), Err(StorageError::Corrupt(_))));
    }
}
