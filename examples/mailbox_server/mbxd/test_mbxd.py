import base64
import hashlib
import hmac
import json
import os
import queue
import sqlite3
import subprocess
import sys
import threading
import time

import pytest

import mbxd

HERE = os.path.dirname(os.path.abspath(__file__))
VECTORS = os.path.join(HERE, '..', '..', '..', 'test', 'rdm_vectors', 'mailbox_conformance.json')

with open(VECTORS) as f:
    CONFORMANCE = json.load(f)

T0 = 1790000000
DAY = 86400
OWNER = hashlib.sha256(b'owner').digest()
K_OWNER = hashlib.sha256(b'k').digest()[:16]
BOB = hashlib.sha256(b'bob').digest()
CAROL = hashlib.sha256(b'carol').digest()


class Clock:
    def __init__(self, t=T0):
        self.t = t

    def __call__(self):
        return self.t


def token(sender, k=K_OWNER):
    return hmac.new(k, sender, hashlib.sha256).digest()[:8]


# Request lines are built here, independent of mbxd, from the format in 06-implementatieplan-v1.md par. 3.16.
def store_line(rid, owner4, sender, pkt_hash, payload):
    return f'@MBX STORE {rid} {owner4} {sender} {pkt_hash} {base64.b64encode(payload).decode()}'


def reg_line(rid, sender, owner4, tok):
    return f'@MBX REG {rid} {sender} {owner4} {tok}'


def fetch_line(rid, client, flags, store_id, reports):
    rep = ','.join(f'{r["hash"]}:{r["result"]}:{r["ack"]}' for r in reports) or '-'
    return f'@MBX FETCH {rid} {client} {flags:02x} {store_id} {rep}'


def stat_line(rid, sender, hashes):
    return f'@MBX STAT {rid} {sender} {",".join(hashes)}'


def one_reply(replies, kind, rid):
    assert len(replies) == 1, replies
    parts = replies[0].split(' ')
    assert parts[0] == 'mbx.' + kind and parts[1] == str(rid), replies[0]
    assert parts[2] == parts[2].lower() and len(parts[2]) == 2, 'code is two lowercase hex digits'
    return parts[2:]


def run_step(mb, clock, rid, step):
    clock.t = step['t']
    op, a, exp = step['op'], step['args'], step.get('expect')
    if op == 'owner_add':
        mb.owner_add(bytes.fromhex(a['pub']), bytes.fromhex(a['k_owner']), a['ttl_days'], a['sync_days'], a['quota'])
    elif op == 'deny':
        mb.deny(bytes.fromhex(a['owner']), bytes.fromhex(a['pub']))
    elif op == 'advance':
        mb.advance()
    elif op == 'reg':
        code, ttl, quota = one_reply(mb.handle_line(reg_line(rid, a['sender'], a['owner'], a['token'])), 'reg', rid)
        assert (int(code, 16), int(ttl), int(quota)) == (exp['code'], exp['ttl_days'], exp['quota'])
    elif op == 'store':
        line = store_line(rid, a['owner'], a['sender'], a['hash'], bytes.fromhex(a['payload']))
        code, expires = one_reply(mb.handle_line(line), 'store', rid)
        assert (int(code, 16), int(expires)) == (exp['code'], exp['expires'])
    elif op == 'fetch':
        line = fetch_line(rid, a['client'], a['flags'], a['store_id'], a['reports'])
        code, remaining, ok, data = one_reply(mb.handle_line(line), 'fetch', rid)
        got = None if data == '-' else base64.b64decode(data, validate=True).hex()
        assert (int(code, 16), int(remaining), int(ok), got) == \
            (exp['code'], exp['remaining'], exp['reports_ok'], exp['payload'])
    elif op == 'stat':
        code, items = one_reply(mb.handle_line(stat_line(rid, a['sender'], a['hashes'])), 'stat', rid)
        assert int(code, 16) == exp['code']
        got = [] if items == '-' else [{'state': int(s), 'ack': k} for s, k in (i.split(':') for i in items.split(','))]
        assert got == exp['items']
    else:
        pytest.fail('unknown op ' + op)


@pytest.fixture
def clock():
    return Clock()


@pytest.fixture
def mb(tmp_path, clock):
    m = mbxd.Mailbox(str(tmp_path / 'mbxd.db'), clock=clock)
    yield m
    m.close()


def registered(mb, clock, sender=BOB, **kw):
    mb.owner_add(OWNER, K_OWNER, **kw)
    r = mb.handle_line(reg_line(1, sender.hex(), OWNER[:4].hex(), token(sender).hex()))
    assert r[0].startswith('mbx.reg 1 00 ')


# ---- conformance ---------------------------------------------------------------------------------------

@pytest.mark.parametrize('case', CONFORMANCE['cases'], ids=[c['name'] for c in CONFORMANCE['cases']])
def test_conformance(tmp_path, case):
    clock = Clock()
    mb = mbxd.Mailbox(str(tmp_path / 'c.db'), clock=clock)
    try:
        for rid, step in enumerate(case['steps']):
            try:
                run_step(mb, clock, rid, step)
            except AssertionError as e:
                raise AssertionError(f'step {rid} ({step["op"]} at t={step["t"]}): {e}') from None
    finally:
        mb.close()


def test_vectors_valid_tokens_match_python_hmac():
    owners = {}
    checked = 0
    for case in CONFORMANCE['cases']:
        for step in case['steps']:
            if step['op'] == 'owner_add':
                owners[step['args']['pub'][:8]] = bytes.fromhex(step['args']['k_owner'])
            if step['op'] == 'reg' and step['expect']['code'] == 0:
                a = step['args']
                assert a['token'] == token(bytes.fromhex(a['sender']), owners[a['owner']]).hex()
                checked += 1
    assert checked > 10


# ---- line parser ---------------------------------------------------------------------------------------

def test_debug_output_and_blank_lines_ignored(mb):
    for line in ['', 'MBX STORE 1', 'mbx.store 1 00 5', '12:00:00 - 27/9/2026 U RAW: 0102', '@MBXSTORE 1']:
        assert mb.handle_line(line) == []


@pytest.mark.parametrize('line', [
    '@MBX',
    '@MBX NOPE 1',
    '@MBX STORE 1 aabbccdd',
    '@MBX STORE x aabbccdd ' + '00' * 32 + ' ' + '00' * 8 + ' AAAA',
    '@MBX STORE 4294967296 aabbccdd ' + '00' * 32 + ' ' + '00' * 8 + ' AAAA',
    '@MBX STORE -1 aabbccdd ' + '00' * 32 + ' ' + '00' * 8 + ' AAAA',
    '@MBX STORE 1 aabbcc ' + '00' * 32 + ' ' + '00' * 8 + ' AAAA',
    '@MBX STORE 1 aabbccdd ' + '00' * 31 + ' ' + '00' * 8 + ' AAAA',
    '@MBX STORE 1 aabbccdd ' + 'zz' * 32 + ' ' + '00' * 8 + ' AAAA',
    '@MBX STORE 1 aabbccdd ' + '00' * 32 + ' ' + '00' * 8 + ' AAA',
    '@MBX STORE 1 aabbccdd ' + '00' * 32 + ' ' + '00' * 8 + ' A*AA',
    '@MBX STORE 1 aabbccdd ' + '00' * 32 + ' ' + '00' * 8 + ' ',
    '@MBX STORE 1  aabbccdd ' + '00' * 32 + ' ' + '00' * 8 + ' AAAA',
    '@MBX STORE 1 aabbccdd ' + '00' * 32 + ' ' + '00' * 8 + ' AAAA extra',
    '@MBX REG 1 ' + '00' * 32 + ' aabbccdd ' + '00' * 7,
    '@MBX FETCH 1 ' + '00' * 32 + ' 0 00000001 -',
    '@MBX FETCH 1 ' + '00' * 32 + ' 00 0001 -',
    '@MBX FETCH 1 ' + '00' * 32 + ' 00 00000001 ',
    '@MBX FETCH 1 ' + '00' * 32 + ' 00 00000001 ' + '00' * 8 + ':0',
    '@MBX FETCH 1 ' + '00' * 32 + ' 00 00000001 ' + '00' * 8 + ':x:' + '00' * 6,
    '@MBX FETCH 1 ' + '00' * 32 + ' 00 00000001 ' + '00' * 8 + ':256:' + '00' * 6,
    '@MBX FETCH 1 ' + '00' * 32 + ' 00 00000001 ' + ','.join([('%016x:0:' % i) + '00' * 6 for i in range(9)]),
    '@MBX STAT 1 ' + '00' * 32,
    '@MBX STAT 1 ' + '00' * 32 + ' -',
    '@MBX STAT 1 ' + '00' * 32 + ' ' + ','.join(['%016x' % i for i in range(9)]),
    '@MBX STAT 1 ' + '00' * 32 + ' ' + '00' * 8 + ',',
])
def test_malformed_lines_get_no_reply(mb, line):
    assert mb.handle_line(line) == []


def test_carriage_return_and_uppercase_hex_accepted(mb, clock):
    registered(mb, clock)
    line = store_line(7, OWNER[:4].hex().upper(), BOB.hex().upper(), 'AB' * 8, b'x' * 40) + '\r\n'
    assert mb.handle_line(line) == ['mbx.store 7 00 %d' % (T0 + 7 * DAY)]


def test_fetch_reply_is_padded_base64(mb, clock):
    registered(mb, clock)
    payload = bytes(range(41))
    mb.handle_line(store_line(2, OWNER[:4].hex(), BOB.hex(), '11' * 8, payload))
    r = mb.handle_line(fetch_line(3, OWNER.hex(), 0, '00000001', []))
    assert r == ['mbx.fetch 3 00 0 0 ' + base64.b64encode(payload).decode()]
    assert r[0].endswith('=')


def test_lines_processed_in_order_by_serve(mb, clock):
    registered(mb, clock)
    out = []
    lines = [store_line(10, OWNER[:4].hex(), BOB.hex(), '22' * 8, b'a' * 30),
             'debug noise',
             store_line(11, OWNER[:4].hex(), BOB.hex(), '22' * 8, b'a' * 30),
             stat_line(12, BOB.hex(), ['22' * 8])]
    mbxd.serve(mb, lines, out.append)
    assert [l.split(' ')[:3] for l in out] == [['mbx.store', '10', '00'], ['mbx.store', '11', '01'],
                                               ['mbx.stat', '12', '00']]
    assert all(l.endswith('\n') for l in out)


# ---- durability ----------------------------------------------------------------------------------------

def test_synchronous_full(mb):
    assert mb.db.execute('PRAGMA synchronous').fetchone()[0] == 2


class ProcessDied(BaseException):
    pass


class DyingConnection:
    """Wraps the real connection; the process dies exactly when COMMIT is issued."""

    def __init__(self, real):
        self.real = real
        self.commits = 0

    def execute(self, sql, *args):
        if sql.strip().upper() == 'COMMIT':
            raise ProcessDied()
        return self.real.execute(sql, *args)

    def __getattr__(self, name):
        return getattr(self.real, name)


def test_crash_between_insert_and_commit_gives_no_reply_and_no_row(tmp_path, clock):
    path = str(tmp_path / 'crash.db')
    mb = mbxd.Mailbox(path, clock=clock)
    registered(mb, clock)
    real = mb.db
    mb.db = DyingConnection(real)
    out = []
    with pytest.raises(ProcessDied):
        mbxd.serve(mb, [store_line(5, OWNER[:4].hex(), BOB.hex(), '33' * 8, b'p' * 50)], out.append)
    assert out == []
    real.close()   # process death: the open transaction is never committed

    mb2 = mbxd.Mailbox(path, clock=clock)
    try:
        assert mb2.handle_line(stat_line(6, BOB.hex(), ['33' * 8])) == ['mbx.stat 6 00 0:' + '00' * 6]
        assert mb2.handle_line(store_line(7, OWNER[:4].hex(), BOB.hex(), '33' * 8, b'p' * 50)) == \
            ['mbx.store 7 00 %d' % (T0 + 7 * DAY)]
    finally:
        mb2.close()


class FailingCommit(DyingConnection):
    def execute(self, sql, *args):
        if sql.strip().upper() == 'COMMIT':
            raise sqlite3.OperationalError('disk I/O error')
        return self.real.execute(sql, *args)


def test_failed_commit_answers_no_storage_and_rolls_back(tmp_path, clock):
    path = str(tmp_path / 'full.db')
    mb = mbxd.Mailbox(path, clock=clock)
    registered(mb, clock)
    real = mb.db
    mb.db = FailingCommit(real)
    assert mb.handle_line(store_line(5, OWNER[:4].hex(), BOB.hex(), '44' * 8, b'p' * 50)) == ['mbx.store 5 13 0']
    assert mb.handle_line(fetch_line(6, OWNER.hex(), 0, '00000001', [])) == ['mbx.fetch 6 13 0 0 -']
    mb.db = real
    assert mb.handle_line(stat_line(7, BOB.hex(), ['44' * 8])) == ['mbx.stat 7 00 0:' + '00' * 6]
    mb.close()


def test_restart_keeps_state_and_store_id(tmp_path, clock):
    path = str(tmp_path / 'restart.db')
    mb = mbxd.Mailbox(path, clock=clock)
    registered(mb, clock)
    o4 = OWNER[:4].hex()
    mb.handle_line(store_line(2, o4, BOB.hex(), '01' * 8, b'one' * 10))
    mb.handle_line(store_line(3, o4, BOB.hex(), '02' * 8, b'two' * 10))
    assert mb.handle_line(fetch_line(4, OWNER.hex(), 0, '0000abcd', []))[0].startswith('mbx.fetch 4 00 1 0 ')
    ack = 'aa' * 4 + '0000'
    mb.handle_line(fetch_line(5, OWNER.hex(), 2, '0000abcd', [{'hash': '01' * 8, 'result': 0, 'ack': ack}]))
    mb.close()

    clock.t += 60
    mb = mbxd.Mailbox(path, clock=clock)
    assert mb.handle_line(stat_line(6, BOB.hex(), ['01' * 8, '02' * 8])) == \
        ['mbx.stat 6 00 2:%s,1:%s' % (ack, '00' * 6)]
    assert mb.handle_line(reg_line(7, BOB.hex(), o4, token(BOB).hex())) == ['mbx.reg 7 00 7 20']
    # same store_id after the restart: no resync, so only the second message is offered
    r = mb.handle_line(fetch_line(8, OWNER.hex(), 0, '0000abcd', []))
    assert r == ['mbx.fetch 8 00 0 0 ' + base64.b64encode(b'two' * 10).decode()]
    r = mb.handle_line(fetch_line(9, OWNER.hex(), 2, '0000ffff', []))
    assert r == ['mbx.fetch 9 00 2 0 -'], 'new store_id resyncs the ON_RADIO message'
    mb.close()


def test_ciphertext_wiped_when_delivered(mb, clock):
    registered(mb, clock)
    mb.handle_line(store_line(2, OWNER[:4].hex(), BOB.hex(), '05' * 8, b's' * 30))
    mb.handle_line(fetch_line(3, OWNER.hex(), 0, '00000001', []))
    mb.handle_line(fetch_line(4, OWNER.hex(), 0, '00000001', [{'hash': '05' * 8, 'result': 1, 'ack': 'bb' * 6}]))
    assert mb.db.execute('SELECT payload, state FROM messages').fetchall() == [(None, 'delivered')]


# ---- admin ---------------------------------------------------------------------------------------------

def test_owner_add_generates_key_and_rejects_prefix_collision(mb):
    k = mb.owner_add(OWNER)
    assert len(k) == 16
    assert mb.owner_add(OWNER, ttl_days=3) == k, 'updating limits keeps the key'
    other = OWNER[:4] + bytes(28)
    with pytest.raises(ValueError):
        mb.owner_add(other)
    for bad in [dict(ttl_days=0), dict(ttl_days=31), dict(sync_days=0), dict(quota=0), dict(quota=256)]:
        with pytest.raises(ValueError):
            mb.owner_add(OWNER, **bad)
    with pytest.raises(ValueError):
        mb.owner_add(OWNER, k_owner=b'short')


def test_deny_removes_registration_and_undeny_allows_again(mb, clock):
    registered(mb, clock)
    mb.deny(OWNER[:4], BOB)
    assert mb.handle_line(reg_line(2, BOB.hex(), OWNER[:4].hex(), token(BOB).hex())) == ['mbx.reg 2 10 0 0']
    mb.undeny(OWNER[:4], BOB)
    assert mb.handle_line(store_line(3, OWNER[:4].hex(), BOB.hex(), '06' * 8, b'x' * 30)) == ['mbx.store 3 10 0'], \
        'deny dropped the registration'
    assert mb.handle_line(reg_line(4, BOB.hex(), OWNER[:4].hex(), token(BOB).hex())) == ['mbx.reg 4 00 7 20']


def test_hello_announces_owners_first_then_recent_depositors(mb, clock):
    owner2 = hashlib.sha256(b'owner2').digest()
    mb.owner_add(OWNER, K_OWNER)
    clock.t += 1
    mb.owner_add(owner2, K_OWNER)
    senders = [hashlib.sha256(b'd%d' % i).digest() for i in range(70)]
    for i, s in enumerate(senders):
        clock.t += 1
        mb.handle_line(reg_line(i, s.hex(), OWNER[:4].hex(), token(s).hex()))
    clock.t += 1
    mb.handle_line(reg_line(99, senders[0].hex(), owner2[:4].hex(), token(senders[0]).hex()))   # most recent
    mb.deny(OWNER[:4], senders[69])

    out = mb.handle_line('@MBX HELLO 1.0-rdm')
    assert out[-2:] == ['mbx.ready', 'mbx.time %d' % clock.t]
    acl = [l.split(' ') for l in out[:-2]]
    assert len(acl) == 64
    assert acl[0] == ['mbx.acl', OWNER.hex(), 'o', OWNER[:4].hex()]
    assert acl[1] == ['mbx.acl', owner2.hex(), 'o', owner2[:4].hex()]
    assert acl[2] == ['mbx.acl', senders[0].hex(), 'd', owner2[:4].hex()]
    assert acl[3] == ['mbx.acl', senders[68].hex(), 'd', OWNER[:4].hex()], 'denied depositor left out'
    pubs = [a[1] for a in acl]
    assert len(set(pubs)) == 64, 'one line per pubkey'


def test_hello_without_owners_is_ready_and_time(mb):
    assert mb.handle_line('@MBX HELLO') == ['mbx.ready', 'mbx.time %d' % T0]


def test_time_every_600_s_after_hello_only(mb, clock):
    out = []
    mbxd.serve(mb, [None], out.append)
    clock.t += 3600
    mbxd.serve(mb, [None, stat_line(1, BOB.hex(), ['01' * 8])], out.append)
    assert out == ['mbx.stat 1 00 0:%s\n' % ('00' * 6)], 'no mbx.time before HELLO'

    out.clear()
    mbxd.serve(mb, ['@MBX HELLO'], out.append)
    hello_t = clock.t
    assert out == ['mbx.ready\n', 'mbx.time %d\n' % hello_t]
    out.clear()
    clock.t = hello_t + 599
    mbxd.serve(mb, [None, stat_line(2, BOB.hex(), ['01' * 8])], out.append)
    assert out == ['mbx.stat 2 00 0:%s\n' % ('00' * 6)]
    out.clear()
    clock.t = hello_t + 600
    mbxd.serve(mb, [stat_line(3, BOB.hex(), ['01' * 8]), None], out.append)
    assert out == ['mbx.stat 3 00 0:%s\n' % ('00' * 6), 'mbx.time %d\n' % (hello_t + 600)]
    out.clear()
    clock.t = hello_t + 1199
    mbxd.serve(mb, [None], out.append)
    assert out == []
    clock.t = hello_t + 1300
    mbxd.serve(mb, [None], out.append)
    assert out == ['mbx.time %d\n' % (hello_t + 1300)]


def test_rotation_survives_restart(tmp_path, clock):
    path = str(tmp_path / 'rot.db')
    mb = mbxd.Mailbox(path, clock=clock)
    registered(mb, clock)
    mb.handle_line(reg_line(2, CAROL.hex(), OWNER[:4].hex(), token(CAROL).hex()))
    first, second = sorted([BOB, CAROL])
    mb.handle_line(store_line(3, OWNER[:4].hex(), first.hex(), '01' * 8, b'first' * 4))
    mb.handle_line(store_line(4, OWNER[:4].hex(), first.hex(), '02' * 8, b'first-again'))
    mb.handle_line(store_line(5, OWNER[:4].hex(), second.hex(), '03' * 8, b'second' * 4))
    assert mb.handle_line(fetch_line(6, OWNER.hex(), 0, '00000001', []))[0].endswith(
        base64.b64encode(b'first' * 4).decode())
    mb.close()
    mb = mbxd.Mailbox(path, clock=clock)
    r = mb.handle_line(fetch_line(7, OWNER.hex(), 0, '00000001', [{'hash': '01' * 8, 'result': 0, 'ack': '11' * 6}]))
    assert r == ['mbx.fetch 7 00 1 1 ' + base64.b64encode(b'second' * 4).decode()]
    mb.close()


def test_opens_database_without_rotation_column(tmp_path, clock):
    path = str(tmp_path / 'old.db')
    db = sqlite3.connect(path)
    db.executescript(mbxd.SCHEMA.replace(', last_sender BLOB', ''))
    db.close()
    mb = mbxd.Mailbox(path, clock=clock)
    registered(mb, clock)
    mb.handle_line(store_line(2, OWNER[:4].hex(), BOB.hex(), '01' * 8, b'x' * 20))
    assert mb.handle_line(fetch_line(3, OWNER.hex(), 0, '00000001', []))[0].startswith('mbx.fetch 3 00 0 0 ')
    mb.close()


# ---- process level -------------------------------------------------------------------------------------

def run_cli(tmp_path, *args, stdin=None):
    return subprocess.run([sys.executable, os.path.join(HERE, 'mbxd.py'), '--db', str(tmp_path / 'cli.db'), *args],
                          input=stdin, capture_output=True, text=True, timeout=30)


def test_cli_owner_add_list_and_deny(tmp_path):
    r = run_cli(tmp_path, 'owner-add', OWNER.hex(), '--ttl-days', '5', '--quota', '9')
    assert r.returncode == 0, r.stderr
    k = r.stdout.split('k_owner ')[1].split()[0]
    assert len(bytes.fromhex(k)) == 16
    r = run_cli(tmp_path, 'owner-list')
    assert OWNER.hex() in r.stdout and 'ttl_days=5' in r.stdout and 'quota=9' in r.stdout
    assert k not in r.stdout, 'listing does not print secrets'
    r = run_cli(tmp_path, 'deny', OWNER[:4].hex(), BOB.hex())
    assert r.returncode == 0, r.stderr
    r = run_cli(tmp_path, 'deny', 'ffffffff', BOB.hex())
    assert r.returncode != 0


def test_stdio_mode_with_fake_clock(tmp_path):
    k = K_OWNER.hex()
    assert run_cli(tmp_path, 'owner-add', OWNER.hex(), '--k-owner', k).returncode == 0
    o4 = OWNER[:4].hex()
    lines = [
        '@MBX HELLO test',
        '@MBX TIME %d' % T0,
        reg_line(1, BOB.hex(), o4, token(BOB).hex()),
        store_line(2, o4, BOB.hex(), '07' * 8, b'z' * 20),
        '@MBX TIME %d' % (T0 + 7 * DAY),
        stat_line(3, BOB.hex(), ['07' * 8]),
    ]
    r = run_cli(tmp_path, 'serve', '--stdio', '--fake-clock', stdin='\n'.join(lines) + '\n')
    assert r.returncode == 0, r.stderr
    assert r.stdout.splitlines() == [
        'mbx.acl %s o %s' % (OWNER.hex(), o4),
        'mbx.ready',
        'mbx.time 0',
        'mbx.time %d' % T0,
        'mbx.reg 1 00 7 20',
        'mbx.store 2 00 %d' % (T0 + 7 * DAY),
        'mbx.time %d' % (T0 + 7 * DAY),
        'mbx.stat 3 00 5:' + '00' * 6,
    ]


def test_time_line_ignored_without_fake_clock(tmp_path):
    r = run_cli(tmp_path, 'serve', '--stdio', stdin='@MBX TIME 5\n@MBX HELLO\n')
    assert r.returncode == 0, r.stderr
    lines = r.stdout.splitlines()
    assert lines[0] == 'mbx.ready' and len(lines) == 2
    assert abs(int(lines[1].split(' ')[1]) - time.time()) < 60, 'real clock, not the ignored TIME line'


def test_stdio_answers_each_line_before_eof(tmp_path):
    p = subprocess.Popen([sys.executable, os.path.join(HERE, 'mbxd.py'), '--db', str(tmp_path / 'live.db'),
                          'serve', '--stdio'], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         stderr=subprocess.DEVNULL, text=True, bufsize=1)
    lines = queue.Queue()
    threading.Thread(target=lambda: [lines.put(l) for l in p.stdout], daemon=True).start()

    def readline():
        try:
            return lines.get(timeout=10)
        except queue.Empty:
            raise AssertionError('no reply within 10 s') from None
    try:
        p.stdin.write('@MBX HELLO\n')
        p.stdin.flush()
        assert readline() == 'mbx.ready\n'
        assert readline().startswith('mbx.time ')
        p.stdin.write(stat_line(1, BOB.hex(), ['01' * 8]) + '\n')
        p.stdin.flush()
        assert readline() == 'mbx.stat 1 00 0:%s\n' % ('00' * 6)
    finally:
        p.stdin.close()
        if p.wait(timeout=10) != 0:
            p.kill()
