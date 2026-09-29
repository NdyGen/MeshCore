#!/usr/bin/env python3
"""mbxd: message store of the MeshCore DM mailbox (examples/mailbox_server), running on the mailbox's own Pi.

The radio does crypto, peer cache and airtime; mbxd is the source of truth for messages, owners, depositor
registrations, tokens and the denylist. Line protocol: docs/reliable-dm/06-implementatieplan-v1.md par. 3.16.
Semantics: README.md next to this file, pinned by test/rdm_vectors/mailbox_conformance.json.
"""
import argparse
import base64
import binascii
import hashlib
import hmac
import logging
import re
import secrets
import sqlite3
import sys
import time

log = logging.getLogger('mbxd')

DAY = 86400
RETAIN_S = 30 * DAY
MAX_INNER = 164
MAX_BATCH = 8
ACL_MAX = 64
MAX_TTL_DAYS = 30
TIME_INTERVAL_S = 600
FETCH_FLAG_NO_PAYLOAD = 0x02
LINE_MAX = 4096

OK, ALREADY_STORED, NOT_AUTH, UNKNOWN_OWNER, QUOTA, NO_STORAGE, TOO_BIG = 0x00, 0x01, 0x10, 0x11, 0x12, 0x13, 0x14
R_ON_RADIO, R_SYNCED, R_DUPLICATE, R_UNDECRYPTABLE, R_INBOX_FULL = range(5)

# SENT is internal to mbxd: a copy went out in a FETCH reply and awaits the next FETCH's report.
STORED, SENT, ON_RADIO, DELIVERED, REJECTED, EXPIRED, SYNC_EXPIRED = (
    'stored', 'sent', 'on_radio', 'delivered', 'rejected', 'expired', 'sync_expired')
LIVE = (STORED, SENT, ON_RADIO)
WIRE_STATE = {STORED: 1, SENT: 1, ON_RADIO: 2, DELIVERED: 3, REJECTED: 4, EXPIRED: 5, SYNC_EXPIRED: 6}
NO_ACK = bytes(6)

SCHEMA = """
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
"""

_U32 = re.compile(r'[0-9]{1,10}\Z')
_HEX = re.compile(r'[0-9a-fA-F]*\Z')


def _u32(s):
    if not _U32.match(s) or int(s) > 0xFFFFFFFF:
        raise ValueError('bad u32 %r' % s)
    return int(s)


def _hex(s, n):
    if len(s) != 2 * n or not _HEX.match(s):
        raise ValueError('expected %d hex bytes, got %r' % (n, s[:80]))
    return bytes.fromhex(s)


def _b64(s):
    if not s:
        raise ValueError('empty payload')
    try:
        return base64.b64decode(s, validate=True)
    except binascii.Error as e:
        raise ValueError('bad base64: %s' % e) from None


def _fields(parts, n):
    if len(parts) != n:
        raise ValueError('expected %d fields, got %d' % (n, len(parts)))
    return parts


def _parse_reports(s):
    if s == '-':
        return []
    reports = []
    for item in s.split(','):
        h, r, a = _fields(item.split(':'), 3)
        if not _U32.match(r) or int(r) > 255:
            raise ValueError('bad report result %r' % r)
        reports.append((_hex(h, 8), int(r), _hex(a, 6)))
    if len(reports) > MAX_BATCH:
        raise ValueError('more than %d reports' % MAX_BATCH)
    return reports


def _parse_hashes(s):
    hashes = [_hex(h, 8) for h in s.split(',')]
    if len(hashes) > MAX_BATCH:
        raise ValueError('more than %d hashes' % MAX_BATCH)
    return hashes


def _token(k_owner, sender):
    return hmac.new(k_owner, sender, hashlib.sha256).digest()[:8]


class Mailbox:
    def __init__(self, db_path, clock=time.time):
        self.clock = clock
        # Autocommit mode: every operation runs in an explicit BEGIN IMMEDIATE ... COMMIT, so a reply is only
        # produced after its changes are durable.
        self.db = sqlite3.connect(db_path, isolation_level=None)
        self.db.execute('PRAGMA journal_mode=WAL')
        self.db.execute('PRAGMA synchronous=FULL')
        self.db.executescript(SCHEMA)
        if 'last_sender' not in [r[1] for r in self.db.execute('PRAGMA table_info(owners)')]:
            self.db.execute('ALTER TABLE owners ADD COLUMN last_sender BLOB')
        self._time_sent = None   # clock value of the last mbx.time; None until the radio said HELLO
        self._commands = {'STORE': self._cmd_store, 'REG': self._cmd_reg, 'FETCH': self._cmd_fetch,
                          'STAT': self._cmd_stat, 'HELLO': self._cmd_hello}

    def close(self):
        self.db.close()

    def now(self):
        return int(self.clock())

    # ---- transactions ----------------------------------------------------------------------------------

    def _tx(self, fn, *args):
        self.db.execute('BEGIN IMMEDIATE')
        try:
            result = fn(*args)
            self.db.execute('COMMIT')
        except sqlite3.Error:
            try:
                self.db.execute('ROLLBACK')
            except sqlite3.Error:
                pass
            raise
        return result

    def _tx_or(self, fallback, fn, *args):
        try:
            return self._tx(fn, *args)
        except sqlite3.Error as e:
            log.error('storage error, answering NO_STORAGE: %s', e)
            return fallback

    def _housekeeping(self, now):
        # final_at is the moment the timer ran out, not the moment we noticed, so retention does not depend on
        # how often lines arrive.
        self.db.execute('UPDATE messages SET state=?, payload=NULL, final_at=t_radio '
                        'WHERE state IN (?,?) AND t_radio <= ?', (EXPIRED, STORED, SENT, now))
        self.db.execute('UPDATE messages SET state=?, payload=NULL, final_at=t_sync '
                        'WHERE state=? AND t_sync <= ?', (SYNC_EXPIRED, ON_RADIO, now))
        self.db.execute('DELETE FROM messages WHERE final_at IS NOT NULL AND final_at + ? <= ?', (RETAIN_S, now))

    def _owner_by4(self, owner4):
        return self.db.execute('SELECT pubkey, owner4, k_owner, ttl_days, sync_days, quota, store_id '
                               'FROM owners WHERE owner4=?', (owner4,)).fetchone()

    def _denied(self, owner4, pub):
        return self.db.execute('SELECT 1 FROM denylist WHERE owner4=? AND pubkey=?', (owner4, pub)).fetchone()

    # ---- line protocol ---------------------------------------------------------------------------------

    def handle_line(self, line):
        """One line from the radio; returns the reply lines (without newline). Malformed lines get no reply,
        the radio then times out after 3 s."""
        line = line.rstrip('\r\n')
        if not line.startswith('@MBX '):
            return []
        parts = line.split(' ')
        handler = self._commands.get(parts[1])
        if handler is None:
            log.warning('unknown command: %s', line[:120])
            return []
        try:
            return handler(parts[2:])
        except ValueError as e:
            log.warning('malformed line ignored (%s): %s', e, line[:120])
            return []

    def _cmd_store(self, f):
        rid, owner4, sender, pkt_hash, payload = _fields(f, 5)
        rid, owner4, sender, pkt_hash = _u32(rid), _hex(owner4, 4), _hex(sender, 32), _hex(pkt_hash, 8)
        payload = _b64(payload)
        code, expires = self._tx_or((NO_STORAGE, 0), self._store, self.now(), owner4, sender, pkt_hash, payload)
        return ['mbx.store %d %02x %d' % (rid, code, expires)]

    def _cmd_reg(self, f):
        rid, sender, owner4, tok = _fields(f, 4)
        rid, sender, owner4, tok = _u32(rid), _hex(sender, 32), _hex(owner4, 4), _hex(tok, 8)
        code, ttl, quota = self._tx_or((NO_STORAGE, 0, 0), self._reg, self.now(), sender, owner4, tok)
        return ['mbx.reg %d %02x %d %d' % (rid, code, ttl, quota)]

    def _cmd_fetch(self, f):
        rid, client, flags, store_id, reports = _fields(f, 5)
        rid, client, flags = _u32(rid), _hex(client, 32), _hex(flags, 1)[0]
        store_id = int.from_bytes(_hex(store_id, 4), 'big')
        reports = _parse_reports(reports)
        code, remaining, ok, payload = self._tx_or((NO_STORAGE, 0, 0, None), self._fetch, self.now(), client,
                                                   flags, store_id, reports)
        data = base64.b64encode(payload).decode() if payload else '-'
        return ['mbx.fetch %d %02x %d %d %s' % (rid, code, remaining, ok, data)]

    def _cmd_stat(self, f):
        rid, sender, hashes = _fields(f, 3)
        rid, sender, hashes = _u32(rid), _hex(sender, 32), _parse_hashes(hashes)
        code, items = self._tx_or((NO_STORAGE, []), self._stat, self.now(), sender, hashes)
        body = ','.join('%d:%s' % (s, a.hex()) for s, a in items) or '-'
        return ['mbx.stat %d %02x %s' % (rid, code, body)]

    def _cmd_hello(self, f):
        log.info('radio hello, firmware %s', ' '.join(f) or '?')
        self._time_sent = self.now()
        return ['mbx.acl %s %s %s' % (pub.hex(), role, owner4.hex()) for pub, role, owner4 in self.acl()] + \
            ['mbx.ready', 'mbx.time %d' % self._time_sent]

    def tick(self):
        """Periodic lines: the radio derives ttl_s and tijd_M from mbx.time, so it follows the Pi clock."""
        now = self.now()
        if self._time_sent is None or now < self._time_sent + TIME_INTERVAL_S:
            return []
        self._time_sent = now
        return ['mbx.time %d' % now]

    # ---- operations ------------------------------------------------------------------------------------

    def _store(self, now, owner4, sender, pkt_hash, payload):
        self._housekeeping(now)
        if len(payload) > MAX_INNER:
            return TOO_BIG, 0
        owner = self._owner_by4(owner4)
        if owner is None:
            return UNKNOWN_OWNER, 0
        registered = self.db.execute('SELECT 1 FROM acl WHERE pubkey=? AND owner4=?', (sender, owner4)).fetchone()
        if self._denied(owner4, sender) or not registered:
            return NOT_AUTH, 0
        row = self.db.execute('SELECT owner4, sender, t_radio FROM messages WHERE pkt_hash=?', (pkt_hash,)).fetchone()
        if row is not None:
            if row[0] == owner4 and row[1] == sender:
                return ALREADY_STORED, row[2]
            return NOT_AUTH, 0
        live = self.db.execute('SELECT COUNT(*) FROM messages WHERE owner4=? AND sender=? AND state IN (?,?,?)',
                               (owner4, sender) + LIVE).fetchone()[0]
        if live >= owner[5]:
            return QUOTA, 0
        expires = now + owner[3] * DAY
        self.db.execute('INSERT INTO messages (pkt_hash, owner4, sender, payload, created, t_radio, state) '
                        'VALUES (?,?,?,?,?,?,?)', (pkt_hash, owner4, sender, payload, now, expires, STORED))
        self.db.execute('UPDATE acl SET sent_count=sent_count+1, last_seen=? WHERE pubkey=? AND owner4=?',
                        (now, sender, owner4))
        return OK, expires

    def _reg(self, now, sender, owner4, tok):
        self._housekeeping(now)
        owner = self._owner_by4(owner4)
        if owner is None:
            return UNKNOWN_OWNER, 0, 0
        if self._denied(owner4, sender) or not hmac.compare_digest(tok, _token(owner[2], sender)):
            return NOT_AUTH, 0, 0
        self.db.execute('INSERT INTO acl (pubkey, owner4, created, last_seen) VALUES (?,?,?,?) '
                        'ON CONFLICT (pubkey, owner4) DO UPDATE SET last_seen=excluded.last_seen',
                        (sender, owner4, now, now))
        return OK, owner[3], owner[5]

    def _fetch(self, now, client, flags, store_id, reports):
        self._housekeeping(now)
        owner = self.db.execute('SELECT owner4, ttl_days, sync_days, store_id, last_sender FROM owners WHERE pubkey=?',
                                (client,)).fetchone()
        if owner is None:
            return NOT_AUTH, 0, 0, None
        owner4, ttl_days, sync_days, last_store_id, last_sender = owner
        if last_store_id is not None and last_store_id != store_id:
            # Alice's radio recreated its RDM storage: everything it may have lost goes out again (G12). T_radio
            # starts again: it is about reaching Alice's radio, which had already happened once.
            n = self.db.execute('UPDATE messages SET state=?, t_radio=?, t_sync=NULL, ack_r=NULL '
                                'WHERE owner4=? AND state IN (?,?)',
                                (STORED, now + ttl_days * DAY, owner4, ON_RADIO, SENT)).rowcount
            log.info('owner %s store_id %08x -> %08x, %d messages back to stored', owner4.hex(), last_store_id,
                     store_id, n)
        self.db.execute('UPDATE owners SET store_id=? WHERE owner4=?', (store_id, owner4))
        for pkt_hash, result, ack in reports:
            self._apply_report(now, owner4, sync_days, pkt_hash, result, ack)
        # A copy the next FETCH does not report on never reached Alice.
        self.db.execute('UPDATE messages SET state=? WHERE owner4=? AND state=?', (STORED, owner4, SENT))
        payload = None
        if not flags & FETCH_FLAG_NO_PAYLOAD:
            sender = self._next_sender(owner4, last_sender)
            if sender is not None:
                row = self.db.execute('SELECT rowid, payload FROM messages WHERE owner4=? AND state=? AND sender=? '
                                      'ORDER BY created, rowid LIMIT 1', (owner4, STORED, sender)).fetchone()
                self.db.execute('UPDATE messages SET state=? WHERE rowid=?', (SENT, row[0]))
                self.db.execute('UPDATE owners SET last_sender=? WHERE owner4=?', (sender, owner4))
                payload = row[1]
        remaining = self.db.execute('SELECT COUNT(*) FROM messages WHERE owner4=? AND state=?',
                                    (owner4, STORED)).fetchone()[0]
        return OK, min(remaining, 255), len(reports), payload

    def _next_sender(self, owner4, last_sender):
        # Senders take turns in pubkey order, so one sender with a full quota does not hold up the others.
        senders = [r[0] for r in self.db.execute('SELECT DISTINCT sender FROM messages WHERE owner4=? AND state=? '
                                                 'ORDER BY sender', (owner4, STORED))]
        if not senders:
            return None
        if last_sender is not None:
            for s in senders:
                if s > last_sender:
                    return s
        return senders[0]

    def _apply_report(self, now, owner4, sync_days, pkt_hash, result, ack):
        # Reports for unknown hashes, other owners' messages or transitions that do not fit are confirmed
        # anyway, otherwise Alice would repeat them forever.
        row = self.db.execute('SELECT state FROM messages WHERE pkt_hash=? AND owner4=?', (pkt_hash, owner4)).fetchone()
        if row is None:
            log.info('report for unknown hash %s', pkt_hash.hex())
            return
        state = row[0]
        if result in (R_ON_RADIO, R_DUPLICATE) and state in (STORED, SENT):
            self.db.execute('UPDATE messages SET state=?, ack_r=?, t_sync=? WHERE pkt_hash=?',
                            (ON_RADIO, ack, now + sync_days * DAY, pkt_hash))
        elif result == R_SYNCED and state in (STORED, SENT, ON_RADIO, SYNC_EXPIRED):
            self.db.execute('UPDATE messages SET state=?, ack_s=?, payload=NULL, final_at=? WHERE pkt_hash=?',
                            (DELIVERED, ack, now, pkt_hash))
        elif result == R_UNDECRYPTABLE and state in (STORED, SENT):
            self.db.execute('UPDATE messages SET state=?, payload=NULL, final_at=? WHERE pkt_hash=?',
                            (REJECTED, now, pkt_hash))
        elif result == R_INBOX_FULL and state == SENT:
            self.db.execute('UPDATE messages SET state=? WHERE pkt_hash=?', (STORED, pkt_hash))
        else:
            log.info('report %d ignored for %s in state %s', result, pkt_hash.hex(), state)

    def _stat(self, now, sender, hashes):
        self._housekeeping(now)
        items = []
        for pkt_hash in hashes:
            row = self.db.execute('SELECT state, ack_r, ack_s FROM messages WHERE pkt_hash=? AND sender=?',
                                  (pkt_hash, sender)).fetchone()
            if row is None:
                items.append((0, NO_ACK))
                continue
            state, ack_r, ack_s = row
            ack = {ON_RADIO: ack_r, SYNC_EXPIRED: ack_r, DELIVERED: ack_s}.get(state)
            items.append((WIRE_STATE[state], ack or NO_ACK))
        return OK, items

    # ---- administration (Pi side) ----------------------------------------------------------------------

    def acl(self):
        """Radio cache fill after HELLO: owners first, then the most recently active depositors, one per pubkey."""
        out = [(pub, 'o', owner4) for pub, owner4 in
               self.db.execute('SELECT pubkey, owner4 FROM owners ORDER BY created, rowid')]
        seen = {pub for pub, _, _ in out}
        rows = self.db.execute(
            'SELECT a.pubkey, a.owner4 FROM acl a JOIN owners o ON o.owner4=a.owner4 '
            'WHERE NOT EXISTS (SELECT 1 FROM denylist d WHERE d.owner4=a.owner4 AND d.pubkey=a.pubkey) '
            'ORDER BY a.last_seen DESC, a.rowid DESC')
        for pub, owner4 in rows:
            if len(out) >= ACL_MAX:
                break
            if pub not in seen:
                seen.add(pub)
                out.append((pub, 'd', owner4))
        return out[:ACL_MAX]

    def owner_add(self, pub, k_owner=None, ttl_days=7, sync_days=30, quota=20):
        """Adds an owner or updates its limits; returns K_owner (kept unless a new one is given)."""
        if len(pub) != 32:
            raise ValueError('owner pubkey must be 32 bytes')
        if k_owner is not None and len(k_owner) != 16:
            raise ValueError('K_owner must be 16 bytes')
        if not 1 <= ttl_days <= MAX_TTL_DAYS or not 1 <= sync_days <= 255 or not 1 <= quota <= 255:
            raise ValueError('ttl_days 1-%d, sync_days 1-255, quota 1-255' % MAX_TTL_DAYS)

        def add():
            row = self.db.execute('SELECT pubkey, k_owner FROM owners WHERE owner4=?', (pub[:4],)).fetchone()
            if row is not None and row[0] != pub:
                raise ValueError('another owner has prefix %s' % pub[:4].hex())
            k = k_owner or (row[1] if row else secrets.token_bytes(16))
            self.db.execute('INSERT INTO owners (pubkey, owner4, k_owner, ttl_days, sync_days, quota, created) '
                            'VALUES (?,?,?,?,?,?,?) ON CONFLICT (pubkey) DO UPDATE SET k_owner=excluded.k_owner, '
                            'ttl_days=excluded.ttl_days, sync_days=excluded.sync_days, quota=excluded.quota',
                            (pub, pub[:4], k, ttl_days, sync_days, quota, self.now()))
            return k
        return self._tx_admin(add)

    def owners(self):
        return self.db.execute('SELECT pubkey, owner4, ttl_days, sync_days, quota, store_id FROM owners '
                               'ORDER BY created, rowid').fetchall()

    def deny(self, owner4, pub):
        def add():
            if self._owner_by4(owner4) is None:
                raise ValueError('unknown owner %s' % owner4.hex())
            self.db.execute('INSERT OR IGNORE INTO denylist (owner4, pubkey, created) VALUES (?,?,?)',
                            (owner4, pub, self.now()))
            self.db.execute('DELETE FROM acl WHERE owner4=? AND pubkey=?', (owner4, pub))
        self._tx_admin(add)

    def undeny(self, owner4, pub):
        self._tx_admin(lambda: self.db.execute('DELETE FROM denylist WHERE owner4=? AND pubkey=?', (owner4, pub)))

    def advance(self):
        self._tx(self._housekeeping, self.now())

    def _tx_admin(self, fn):
        self.db.execute('BEGIN IMMEDIATE')
        try:
            result = fn()
        except BaseException:
            self.db.execute('ROLLBACK')
            raise
        self.db.execute('COMMIT')
        return result


def serve(mailbox, lines, write):
    """lines yields input lines, or None when there was no input for a while (so periodic lines still go out)."""
    for line in lines:
        replies = mailbox.handle_line(line) if line is not None else []
        for reply in replies + mailbox.tick():
            write(reply + '\n')


class FakeClock:
    """Test clock for --fake-clock: '@MBX TIME <unix>' sets it (simulator fast-forward)."""

    def __init__(self):
        self.t = 0

    def __call__(self):
        return self.t


def run_stdio(mailbox, fake_clock):
    def lines():
        for line in sys.stdin:
            if fake_clock is not None and line.startswith('@MBX TIME '):
                try:
                    fake_clock.t = _u32(line[len('@MBX TIME '):].strip())
                except ValueError:
                    log.warning('bad time line: %s', line.strip()[:80])
                yield None
                continue
            yield line

    def write(s):
        sys.stdout.write(s)
        sys.stdout.flush()
    serve(mailbox, lines(), write)


def run_serial(mailbox, port):
    import serial   # only needed on the Pi

    ser = serial.Serial()
    ser.port, ser.baudrate, ser.timeout = port, 115200, 1.0
    ser.dtr = ser.rts = False
    ser.open()   # resets the ESP32; the radio answers with @MBX HELLO after boot

    def lines():
        buf = b''
        while True:
            data = ser.read(ser.in_waiting or 1)
            if not data:
                yield None
                continue
            buf += data
            while b'\n' in buf:
                line, buf = buf.split(b'\n', 1)
                yield line.decode(errors='replace')
            if len(buf) > LINE_MAX:
                log.warning('dropping %d bytes without newline', len(buf))
                buf = b''

    serve(mailbox, lines(), lambda s: ser.write(s.encode()))


def _owner4_arg(s):
    if len(s) == 64:
        return _hex(s, 32)[:4]
    return _hex(s, 4)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--db', default='/var/lib/mbxd/mbxd.db')
    ap.add_argument('-v', '--verbose', action='store_true')
    sub = ap.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('serve', help='answer the radio')
    where = p.add_mutually_exclusive_group(required=True)
    where.add_argument('--port', help='serial port of the mailbox radio')
    where.add_argument('--stdio', action='store_true', help='lines on stdin, replies on stdout (tests)')
    p.add_argument('--fake-clock', action='store_true', help="with --stdio: clock set by '@MBX TIME <unix>'")
    p = sub.add_parser('owner-add', help='add an owner or change its limits; prints K_owner')
    p.add_argument('pub', help='owner public key, 64 hex')
    p.add_argument('--k-owner', help='32 hex; default: keep the current key or generate one')
    p.add_argument('--ttl-days', type=int, default=7)
    p.add_argument('--sync-days', type=int, default=30)
    p.add_argument('--quota', type=int, default=20)
    sub.add_parser('owner-list', help='list owners (without keys)')
    for name in ('deny', 'undeny'):
        p = sub.add_parser(name, help='%s a depositor for one owner' % name)
        p.add_argument('owner', help='owner prefix (8 hex) or public key (64 hex)')
        p.add_argument('pub', help='depositor public key, 64 hex')
    args = ap.parse_args(argv)
    logging.basicConfig(stream=sys.stderr, level=logging.DEBUG if args.verbose else logging.INFO,
                        format='%(asctime)s %(levelname)s %(message)s')

    fake_clock = FakeClock() if args.cmd == 'serve' and args.fake_clock else None
    mailbox = Mailbox(args.db, clock=fake_clock or time.time)
    try:
        if args.cmd == 'serve':
            if args.port:
                run_serial(mailbox, args.port)
            else:
                run_stdio(mailbox, fake_clock)
        elif args.cmd == 'owner-add':
            k = mailbox.owner_add(_hex(args.pub, 32), _hex(args.k_owner, 16) if args.k_owner else None,
                                  args.ttl_days, args.sync_days, args.quota)
            print('owner %s k_owner %s' % (args.pub[:8].lower(), k.hex()))
        elif args.cmd == 'owner-list':
            for pub, owner4, ttl, sync, quota, store_id in mailbox.owners():
                sid = '%08x' % store_id if store_id is not None else '-'
                print('owner %s owner4=%s ttl_days=%d sync_days=%d quota=%d store_id=%s'
                      % (pub.hex(), owner4.hex(), ttl, sync, quota, sid))
        else:
            owner4 = _owner4_arg(args.owner)
            getattr(mailbox, args.cmd)(owner4, _hex(args.pub, 32))
    except ValueError as e:
        print('mbxd: %s' % e, file=sys.stderr)
        return 2
    finally:
        mailbox.close()
    return 0


if __name__ == '__main__':
    sys.exit(main())
