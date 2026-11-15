#!/usr/bin/env python3
"""Grouped paths and exact metadata/content history, through the real view worker."""
from contextlib import closing
from pathlib import Path
import os
import re
import sqlite3
import subprocess as sp
import sys
import tempfile

app, driver = (Path(arg).resolve() for arg in sys.argv[1:3])
with tempfile.TemporaryDirectory(prefix='ccurate-content-history-') as temporary:
    root = Path(temporary)
    source, target = root / 'source', root / 'destination'
    source.mkdir(); target.mkdir(); (target / 'curated').mkdir()
    file = source / 'translucent.jpg'
    payload = b'unchanged content, changed metadata\n'
    file.write_bytes(payload)
    (source / 'backup.jpg').write_bytes(payload)
    for name in ('empty-a', 'empty-b'):
        (source / name).touch()
    database = target / '.ccurate/catalog.sqlite'

    def cli(*args):
        r = sp.run([app, '--cli', source, target, *args], capture_output=True, timeout=60)
        assert r.returncode == 0, r.stderr.decode(errors='replace')
        return r.stdout.decode(errors='replace')

    def query(sql, args=()):
        with closing(sqlite3.connect(database)) as db:
            return db.execute(sql, args).fetchall()

    def inspect(path='translucent.jpg', expanded=1, page=0, occurrences=0, side='source'):
        r = sp.run([driver, target, path, str(occurrences), side, str(expanded), str(page)],
                   capture_output=True, timeout=20, env=dict(os.environ, TZ='UTC'))
        assert r.returncode == 0, r.stderr.decode(errors='replace')
        return r.stdout.decode().replace('\r\n', '\n'), r.stderr.decode()

    def occurrences(text):
        return [line for line in text.splitlines() if line.startswith(('[OK] ', '[ ] '))]

    cli('scan')
    eid = query("SELECT id FROM entries WHERE path='translucent.jpg'")[0][0]
    first = query('SELECT current FROM entries WHERE id=?', (eid,))[0][0]
    cli('scan')
    assert query('SELECT COUNT(*) FROM versions WHERE eid=?', (eid,)) == [(1,)]
    cli('copy', 'translucent.jpg', 'curated')
    # A metadata-only change must still keep precisely one occurrence per path.
    os.utime(file, ns=(file.stat().st_atime_ns, 1700000000123456700))
    if os.name != 'nt':
        file.chmod(0o640)
    cli('scan')
    second, mtime, ctime = query('SELECT id,mtime,ctime FROM versions WHERE eid=? ORDER BY id DESC', (eid,))[0]
    assert first != second and mtime == 1700000000123456700
    text, status = inspect()
    assert len(occurrences(text)) == 2 and '[older version]' not in text
    assert f'v{first} -> v{second}' in text and 'Metadata changed; content unchanged' in text
    assert 'mtime:\n    previous:' in text and '.123456700' in text
    assert 'ctime:\n    previous:' in text
    if os.name != 'nt':
        assert 'Type / permissions:\n    previous:' in text and '100640' in text
    assert 'available=1 more=0' in status
    collapsed, status = inspect(expanded=0)
    assert '2 catalog records; 1 earlier record.' in collapsed
    assert 'Show changes:' in collapsed and ' -> v' not in collapsed
    assert 'expanded=0' in status
    # Identical size and mtime can conceal changed bytes; recorded hashes distinguish them.
    file.write_bytes(b'X' * len(payload))
    os.utime(file, ns=(file.stat().st_atime_ns, mtime))
    cli('scan', '--full')
    third = query('SELECT current FROM entries WHERE id=?', (eid,))[0][0]
    text, _ = inspect()
    assert f'v{second} -> v{third}' in text and 'Content changed (different SHA-512)' in text
    assert 'SHA-512:\n    previous:' in text and 'Size (bytes):' not in text
    old, _ = inspect('backup.jpg')
    assert '[historical content only]' in next(x for x in occurrences(old) if 'translucent.jpg' in x)
    assert len(occurrences(old)) == 2
    destination, _ = inspect('curated/translucent.jpg', side='destination')
    assert 'VERSION HISTORY: translucent.jpg' in destination and f'v{second} -> v{third}' in destination
    assert '[historical content only]' in destination
    print('PASS real scan / unchanged scan reuse / metadata diffs / ns times / content changes / destination provenance')

    # Explicit fixture records exercise every supported field, missing hashes,
    # pre-epoch timestamps, and bounded history pages without changing real files.
    with closing(sqlite3.connect(database)) as db:
        columns = [row[1] for row in db.execute('PRAGMA table_info(versions)') if row[1] != 'id']
        base = dict(zip(columns, db.execute('SELECT ' + ','.join(columns) + ' FROM versions WHERE id=?', (third,)).fetchone()))
        insert = 'INSERT INTO versions (' + ','.join(columns) + ') VALUES (' + ','.join('?' for _ in columns) + ')'
        base.update(uid=123, gid=456, ino=789, dev=5, atime=-1, btime=0, attrs='[{"name":"user.test","value":"new"}]',
                    meta_error='fixture metadata error', hash=None, accepted=0)
        last = db.execute(insert, [base[c] for c in columns]).lastrowid
        for i in range(20):
            base.update(mtime=mtime + i, ctime=ctime + i)
            last = db.execute(insert, [base[c] for c in columns]).lastrowid
        db.execute('UPDATE entries SET current=?,error=? WHERE id=?', (last, 'UNSCANNED: fixture', eid))
        db.execute("UPDATE entries SET reviewed=1,discarded=1,present=0 WHERE path='backup.jpg'")
        db.commit()
    text, status = inspect(page=0)
    assert 'more=1' in status and 'Content comparison unavailable' in text
    assert len(re.findall(r'^v\d', text, re.M)) == 16
    text, status = inspect(page=1)
    assert 'more=0' in status and 'First catalog observation' in text
    assert 'xattrs / ACLs: changed' in text and 'Metadata error:' in text
    # Windows CRT may reject pre-epoch localtime; the numeric fallback stays exact.
    assert ('1969-12-31 23:59:59.999999999' in text or
            (os.name == 'nt' and '-1 ns since Unix epoch' in text))
    assert 'UID:' in text and 'Inode:' in text
    assert inspect(page=999)[0] == text
    assert inspect(page=-1)[0] == inspect(page=0)[0]
    old, _ = inspect('backup.jpg', expanded=0)
    assert '[current content unverified]' in old
    assert '[reviewed] [rejected] [absent at last scan]' in old
    assert len(occurrences(old)) == 2
    empty, _ = inspect('empty-a')
    assert not occurrences(empty) and 'empty-b' not in empty
    print('PASS all stored metadata fields / unknown hashes / history paging / source flags / independent empty files')

    # Occurrence pages must count paths, not historical records. Multiple versions
    # around a page boundary must not hide, repeat, or skip any source path.
    with closing(sqlite3.connect(database)) as db:
        fields = ','.join(columns)
        original = dict(zip(columns, db.execute('SELECT '+fields+' FROM versions WHERE id=?', (first,)).fetchone()))
        for i in range(270):
            path = f'copies/{i:03}.jpg'
            new_eid = db.execute("INSERT INTO entries(path,parent,name,kind,present) VALUES (?,?,?,'file',1)",
                                 (path, 'copies', f'{i:03}.jpg')).lastrowid
            original.update(eid=new_eid)
            for _ in range(3):
                current = db.execute(insert, [original[c] for c in columns]).lastrowid
            db.execute('UPDATE entries SET current=? WHERE id=?', (current, new_eid))
        db.commit()
    def snapshot():
        with closing(sqlite3.connect(database)) as db:
            return {t: db.execute('SELECT * FROM '+t).fetchall()
                    for t in ('entries', 'versions', 'objects', 'empty_copies', 'discoveries', 'events')}
    before = snapshot()
    source.rename(root / 'disconnected-source')
    p1, _ = inspect('backup.jpg', expanded=0)
    p2, _ = inspect('backup.jpg', expanded=0, occurrences=1)
    p3, _ = inspect('backup.jpg', expanded=0, occurrences=2)
    assert len(occurrences(p1)) == 256 and len(occurrences(p2)) == 16
    assert 'More occurrences on the next page.' in p1 and 'More occurrences' not in p2
    rows = occurrences(p1) + occurrences(p2)
    assert len(rows) == len(set(rows)) == 272
    assert 'No occurrences on this page.' in p3
    inspect('curated/translucent.jpg', side='destination')
    assert snapshot() == before, 'Details changed the saved catalog'
    print('PASS 272 unique paths / 3 records per path / offline browsing / catalog unchanged')
