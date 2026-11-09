#!/usr/bin/env python3
"""Build a disposable deep-catalog fixture; no real source data is read or copied.

Usage: navigation_scale.py APP DRIVER DIRECTORY [FILES]
An existing fixture is reused for before/after measurements.
"""
from pathlib import Path
import hashlib
import sqlite3
import subprocess
import sys
import time

app, driver = (str(Path(x).resolve()) for x in sys.argv[1:3])
base = Path(sys.argv[3]).resolve()
files = int(sys.argv[4]) if len(sys.argv) > 4 else 200_000
source, destination = base / 'source', base / 'destination'
if not (base / 'ready').exists():
    source.mkdir(parents=True, exist_ok=True)
    destination.mkdir(parents=True, exist_ok=True)
    for i in range(8):
        (source / f'group-{i:02d}' / 'deep').mkdir(parents=True, exist_ok=True)
    subprocess.run([app, '--cli', str(source), str(destination), 'scan'], check=True)
    started = time.monotonic()
    digest = hashlib.sha512(b'synthetic repeated content').hexdigest()
    with sqlite3.connect(destination / '.ccurate/catalog.sqlite') as db:
        db.execute('PRAGMA journal_mode=OFF')
        db.execute('PRAGMA synchronous=OFF')
        db.execute('PRAGMA cache_size=-131072')
        db.execute('INSERT INTO objects(hash,size) VALUES(?,4096)', (digest,))
        for start in range(0, files, 100_000):
            stop = min(files, start + 100_000)
            db.execute('''WITH RECURSIVE n(i) AS (VALUES(?) UNION ALL
                SELECT i+1 FROM n WHERE i<?)
                INSERT INTO entries(id,path,parent,name,kind,current)
                SELECT i+100,printf('group-%02d/deep/file-%08d',i%8,i),
                  printf('group-%02d/deep',i%8),printf('file-%08d',i),'file',i+100 FROM n''',
                       (start + 1, stop))
            db.execute('''INSERT INTO versions(id,eid,mode,size,hash)
                SELECT id,id,33188,4096,? FROM entries WHERE id>? AND id<=?''',
                       (digest, start + 100, stop + 100))
            db.commit()
            if stop % 1_000_000 == 0 or stop == files:
                print(f'Fixture: {stop:,} files; {time.monotonic()-started:.1f}s', flush=True)
        db.execute('INSERT INTO hash_groups(hash,occurrences) VALUES(?,?)', (digest, files))
        db.execute("INSERT OR REPLACE INTO settings VALUES('hash_groups_ready','2')")
        db.commit()
        db.execute('PRAGMA journal_mode=WAL')
    (base / 'ready').write_text(str(files))
subprocess.run([driver, str(source), str(destination)], check=True, timeout=900)
