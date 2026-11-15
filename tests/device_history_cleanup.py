#!/usr/bin/env python3
"""Offline maintenance preserves content, metadata, retention and destination mappings."""
from contextlib import closing
from pathlib import Path
import base64
import hashlib
import json
import os
import sqlite3
import subprocess as sp
import sys
import tempfile

app = Path(sys.argv[1]).resolve()
script = Path(__file__).resolve().parents[1] / 'scripts/cleanup_device_history.py'
with tempfile.TemporaryDirectory(prefix='ccurate-cleanup-') as temp:
    root = Path(temp); src = root / 'source'; dst = root / 'destination'
    src.mkdir(); dst.mkdir()
    (src / 'file').write_bytes(b'unchanged payload')
    (src / 'empty').touch()
    raw_path = b'raw-\xff' if os.name != 'nt' else b'raw-name'
    with open(os.fsencode(src) + b'/' + raw_path, 'wb') as f:
        f.write(b'raw path data')
    def cli(*args, source=src, target=dst):
        r = sp.run([app, '--cli', source, target, *args], capture_output=True, timeout=30)
        assert not r.returncode, r.stderr.decode(errors='replace')
    cli('scan'); cli('copy', '', 'Curated', '--complete')
    database = dst / '.ccurate/catalog.sqlite'; manifest = dst / '.ccurate/archive.jsonl'
    def snapshot(path=database):
        with closing(sqlite3.connect(path)) as db:
            db.text_factory = bytes
            return {t: db.execute('SELECT * FROM ' + t + ' ORDER BY 1').fetchall()
                    for t in ('entries', 'versions', 'empty_copies', 'rejected_retention', 'objects',
                              'discoveries', 'events', 'source_filesystems', 'hash_groups')}
    def cleanup(*args, ok=True):
        r = sp.run([sys.executable, script, dst, *args], capture_output=True, timeout=60)
        assert (r.returncode == 0) == ok, (r.stdout + r.stderr).decode(errors='replace')
        return r.stdout.decode(), r.stderr.decode()
    with closing(sqlite3.connect(database)) as db, db:
        fields = [r[1] for r in db.execute('PRAGMA table_info(versions)') if r[1] != 'id']
        sql = 'INSERT INTO versions(' + ','.join(fields) + ') VALUES(' + ','.join('?' for _ in fields) + ')'
        def clone(original, **updates):
            values = dict(zip(fields, db.execute('SELECT ' + ','.join(fields) + ' FROM versions WHERE id=?', (original,)).fetchone()))
            values.update(updates)
            return db.execute(sql, [values[k] for k in fields]).lastrowid
        initial = db.execute("SELECT current FROM entries WHERE path='file'").fetchone()[0]
        empty = db.execute("SELECT current FROM entries WHERE path='empty'").fetchone()[0]
        db.execute('UPDATE versions SET dev=53')
        for device in (54, 53, 54):
            last = clone(initial, dev=device)
        db.execute("UPDATE entries SET current=?,reviewed=1,discarded=1 WHERE path='file'", (last,))
        duplicate_empty = clone(empty, dev=54)
        db.execute('INSERT INTO empty_copies SELECT ?,dest,state,verified FROM empty_copies WHERE version=?', (duplicate_empty, empty))
        db.execute('INSERT INTO rejected_retention VALUES(?)', (empty,))
        db.execute('INSERT INTO rejected_retention VALUES(?)', (duplicate_empty,))
        db.execute("UPDATE entries SET current=? WHERE path='empty'", (duplicate_empty,))
        # Same content with different metadata must survive, including precise timestamps.
        modified = clone(initial, dev=54, mtime=1234567890123456789)
        db.execute("UPDATE entries SET current=? WHERE path='file'", (modified,))
        content = clone(modified, dev=53, hash='a' * 128)
        # Two semantically distinct empty copies cannot be collapsed.
        conflict = clone(empty, dev=54)
        db.execute("INSERT INTO empty_copies VALUES(?,'another-empty','verified',1)", (conflict,))
        # A retained vs non-retained observation must survive even if otherwise equal.
        unretained = clone(content, dev=54, accepted=0)
        # Adjacent records for a non-UTF-8 filename are handled as bytes.
        raw_eid, raw = db.execute('SELECT id,current FROM entries WHERE CAST(path AS BLOB)=?', (raw_path,)).fetchone()
        raw_duplicate = clone(raw, dev=54)
        db.execute('UPDATE entries SET current=? WHERE id=?', (raw_duplicate, raw_eid))
    before = snapshot(); old_manifest = manifest.read_bytes()
    out, _ = cleanup()
    assert 'redundant=5' in out, out
    assert snapshot() == before and manifest.read_bytes() == old_manifest
    # A database failure rolls back current mappings, deletes and legacy clearing.
    with closing(sqlite3.connect(database)) as db, db:
        db.execute("CREATE TRIGGER fail_cleanup BEFORE DELETE ON versions BEGIN SELECT RAISE(ABORT,'fixture failure'); END")
    _, error = cleanup('--apply', '--backup', str(root / 'failed-backup.sqlite'), ok=False)
    assert 'fixture failure' in error
    assert snapshot() == before and manifest.read_bytes() == old_manifest
    with closing(sqlite3.connect(database)) as db, db:
        db.execute('DROP TRIGGER fail_cleanup')
    backup = root / 'backup.sqlite'
    out, _ = cleanup('--apply', '--backup', str(backup))
    assert snapshot(backup) == before
    after = snapshot()
    with closing(sqlite3.connect(database)) as db:
        ids = {r[0] for r in db.execute('SELECT id FROM versions')}
        assert {initial, empty, modified, content, conflict, unretained, raw} <= ids
        assert not ({last, duplicate_empty, raw_duplicate} & ids)
        assert db.execute('SELECT COUNT(*) FROM versions WHERE dev IS NOT NULL').fetchone() == (0,)
        assert db.execute("SELECT reviewed,discarded,current FROM entries WHERE path='file'").fetchone() == (1, 1, modified)
        assert db.execute("SELECT current FROM entries WHERE path='empty'").fetchone() == (empty,)
        assert not db.execute('PRAGMA foreign_key_check').fetchall()
    assert before['objects'] == after['objects'] and before['events'] == after['events']
    assert before['hash_groups'] == after['hash_groups']
    records = [json.loads(line) for line in manifest.read_text().splitlines()]
    assert raw_path in {base64.b64decode(r['path_b64']) for r in records if r['type'] == 'entry'}
    assert all('dev' not in r for r in records)
    out, _ = cleanup(); assert 'redundant=0' in out and 'legacy=0' in out
    print('PASS dry-run / backup / device-only merge / real changes kept / flags / empty copies / non-UTF-8 paths / idempotency')
    # Offline use proves the utility does not consult or mutate source data.
    src.rename(root / 'disconnected')
    backup2 = root / 'backup2.sqlite'
    cleanup('--apply', '--backup', str(backup2), '--vacuum')
    assert snapshot() == after
    fresh = root / 'import'; fresh.mkdir()
    cli('import', manifest, source='-', target=fresh)
    print('PASS source disconnected / vacuum / regenerated manifest imports')
    # The same operating-system lock as the application must exclude maintenance.
    sys.path.insert(0, str(script.parent))
    import cleanup_device_history as module
    with module.session_lock(database.parent / 'session.lock'):
        out, err = cleanup(ok=False)
        assert 'Close CCurate' in err
    with closing(sqlite3.connect(database)) as db, db:
        db.execute("INSERT INTO operations(kind,state) VALUES('copy','pending')")
    out, err = cleanup(ok=False); assert 'Pending file operations' in err
    with closing(sqlite3.connect(database)) as db, db:
        db.execute('DELETE FROM operations')
        db.execute('PRAGMA user_version=99')
    out, err = cleanup(ok=False); assert 'Unsupported catalog schema' in err
    with closing(sqlite3.connect(database)) as db, db:
        db.execute('PRAGMA user_version=3')
        db.execute('CREATE TABLE future_ref(version INTEGER REFERENCES versions(id))')
    out, err = cleanup(ok=False); assert 'Unknown reference' in err
    with closing(sqlite3.connect(database)) as db, db:
        db.execute('DROP TABLE future_ref')
        db.execute('ALTER TABLE versions DROP COLUMN filesystem')
        db.execute('ALTER TABLE discoveries DROP COLUMN filesystem')
        db.execute('DROP TABLE source_filesystems')
        db.execute('PRAGMA user_version=2')
        db.execute('UPDATE versions SET dev=53')
    cleanup('--apply', '--backup', str(root / 'schema2-backup.sqlite'))
    assert json.loads(manifest.read_text().splitlines()[0])['schema'] == 2
    print('PASS active-session lock / pending operations / unknown schema/reference refusal / schema-2 offline cleanup')
