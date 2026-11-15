#!/usr/bin/env python3
"""Persistent identity, legacy migration and portable references using real catalogs."""
from contextlib import closing
from pathlib import Path
import json
import sqlite3
import subprocess as sp
import sys
import tempfile

app = Path(sys.argv[1]).resolve()
driver = Path(sys.argv[2]).resolve() if len(sys.argv) > 2 else None
with tempfile.TemporaryDirectory(prefix='ccurate-identity-') as temp:
    root = Path(temp)
    src, dst = root / 'source', root / 'destination'
    src.mkdir(); dst.mkdir(); (src / 'file').write_bytes(b'identity fixture\n')
    def cli(*args, target=dst, ok=True):
        r = sp.run([app, '--cli', src, target, *args], capture_output=True, timeout=30)
        assert (r.returncode == 0) == ok, r.stderr.decode(errors='replace')
        return r
    if driver:
        r = sp.run([driver, src, dst], capture_output=True, timeout=60)
        assert not r.returncode, r.stdout.decode() + r.stderr.decode(errors='replace')
        print(r.stdout.decode(), end='')
    else:
        cli('scan'); cli('copy', 'file', 'Curated')
    database = dst / '.ccurate/catalog.sqlite'
    def dbopen():
        return closing(sqlite3.connect(database))
    # Emulate the actual pre-migration schema, rather than just changing its version pragma.
    with dbopen() as db, db:
        db.execute('ALTER TABLE versions DROP COLUMN filesystem')
        db.execute('ALTER TABLE discoveries DROP COLUMN filesystem')
        db.execute('DROP TABLE source_filesystems')
        db.execute('PRAGMA user_version=2')
        db.execute('UPDATE versions SET dev=53')
        db.execute('UPDATE entries SET reviewed=1')
        before = db.execute('SELECT id,current,reviewed,discarded FROM entries').fetchall()
        kept = db.execute('SELECT id,accepted FROM versions').fetchall()
    cli('export')
    with dbopen() as db:
        assert db.execute('PRAGMA user_version').fetchone() == (3,)
        assert db.execute('SELECT id,current,reviewed,discarded FROM entries').fetchall() == before
        assert db.execute('SELECT id,accepted FROM versions').fetchall() == kept
        assert db.execute('SELECT COUNT(*) FROM source_filesystems').fetchone() == (0,)
    cli('refresh', '')
    with dbopen() as db:
        assert db.execute('SELECT id,current,reviewed,discarded FROM entries').fetchall() == before
        assert db.execute('SELECT id,accepted FROM versions').fetchall() == kept
        assert not db.execute('PRAGMA foreign_key_check').fetchall()
    cli('export')
    manifest = dst / '.ccurate/archive.jsonl'
    records = [json.loads(line) for line in manifest.read_text().splitlines()]
    assert records[0]['schema'] == 3
    identities = {r['id'] for r in records if r['type'] == 'filesystem'}
    for record in records:
        if record['type'] == 'entry':
            assert 'dev' not in record
            assert record['filesystem'] is None or record['filesystem'] in identities
            assert 'identity' not in record
    fresh = root / 'import'; fresh.mkdir()
    cli('import', manifest, target=fresh)
    with closing(sqlite3.connect(fresh / '.ccurate/catalog.sqlite')) as db:
        assert not db.execute('PRAGMA foreign_key_check').fetchall()
        assert db.execute('SELECT COUNT(*) FROM versions WHERE accepted=1').fetchone()[0] == sum(accepted for _, accepted in kept)
    # Old portable indexes still import, with unknown provenance rather than a guessed GUID.
    legacy = root / 'legacy.jsonl'
    old = [dict(r) for r in records if r['type'] != 'filesystem']
    old[0]['schema'] = 2
    for r in old:
        if r['type'] == 'entry':
            r.pop('filesystem', None); r['dev'] = '54'
    legacy.write_text('\n'.join(json.dumps(r) for r in old) + '\n')
    old_dest = root / 'old-import'; old_dest.mkdir(); cli('import', legacy, target=old_dest)
    with closing(sqlite3.connect(old_dest / '.ccurate/catalog.sqlite')) as db:
        assert db.execute('SELECT COUNT(*) FROM versions WHERE dev IS NOT NULL OR filesystem IS NOT NULL').fetchone() == (0,)
    # A dangling reference must roll back the entire import.
    invalid = root / 'bad.jsonl'
    bad = [dict(r) for r in records]
    next(r for r in bad if r['type'] == 'entry')['filesystem'] = 999999
    invalid.write_text('\n'.join(json.dumps(r) for r in bad) + '\n')
    bad_dest = root / 'bad-import'; bad_dest.mkdir(); cli('import', invalid, target=bad_dest, ok=False)
    with closing(sqlite3.connect(bad_dest / '.ccurate/catalog.sqlite')) as db:
        assert db.execute('SELECT COUNT(*) FROM entries').fetchone() == (0,)
        assert db.execute('SELECT COUNT(*) FROM source_filesystems').fetchone() == (0,)
    print('PASS schema-2 upgrade / lazy binding / schema-3 round trip / legacy import / invalid reference rollback')
