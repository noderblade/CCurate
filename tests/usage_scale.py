#!/usr/bin/env python3
"""Optional large catalog regression; no source payloads or existing user data.

python3 tests/usage_scale.py build/ccurate build/usagecheck /path/to/scratch 17000000
The disposable fixture has 1,000 files per directory and cross-directory duplicates.
Allow roughly 1 KiB of scratch space per file for catalog records and indexes.
"""
from pathlib import Path
import json
import os
import sqlite3
import subprocess as sp
import sys
import tempfile
import time

app, driver = (str(Path(p).resolve()) for p in sys.argv[1:3])
parent = Path(sys.argv[3]).resolve()
count = int(sys.argv[4]) if len(sys.argv) > 4 else 1000000
assert count >= 1000 and count % 1000 == 0
parent.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(prefix='ccurate-usage-scale-', dir=parent) as temporary:
    root = Path(temporary)
    source, destination = root / 'source', root / 'destination'
    source.mkdir(); destination.mkdir(); (source / 'data').mkdir()
    sp.run([app, '--cli', str(source), str(destination), 'scan'], check=True, capture_output=True)
    catalog = destination / '.ccurate/catalog.sqlite'
    started = time.monotonic()
    with sqlite3.connect(catalog) as db:
        db.execute('PRAGMA journal_mode=OFF'); db.execute('PRAGMA synchronous=OFF')
        db.execute('PRAGMA cache_size=-65536'); db.execute('PRAGMA temp_store=FILE')
        db.execute("""WITH RECURSIVE n(i) AS (VALUES(0) UNION ALL SELECT i+1 FROM n WHERE i+1<?)
            INSERT INTO entries(id,path,parent,name,kind)
            SELECT 100+i,printf('data/d%06d',i),'data',printf('d%06d',i),'directory' FROM n""",
                   (count // 1000,))
        for start in range(0, count, 100000):
            end = min(start+100000, count)
            db.execute("""WITH RECURSIVE n(i) AS (VALUES(?) UNION ALL SELECT i+1 FROM n WHERE i+1<?)
                INSERT INTO versions(id,eid,mode,size,uid,gid,mtime,ctime,atime,btime,dev,ino,hash)
                SELECT 1000000+i,1000000+i,33188,524288,1000,1000,0,0,0,0,1,i,
                printf('%0128x',i%1000+1) FROM n""", (start, end))
            db.execute("""WITH RECURSIVE n(i) AS (VALUES(?) UNION ALL SELECT i+1 FROM n WHERE i+1<?)
                INSERT INTO entries(id,path,parent,name,kind,current)
                SELECT 1000000+i,printf('data/d%06d/f%04d',i/1000,i%1000),
                printf('data/d%06d',i/1000),printf('f%04d',i%1000),'file',1000000+i FROM n""",
                       (start, end))
            db.commit()
            if end % 1000000 == 0:
                print(f'Fixture: {end:,} / {count:,} files ({time.monotonic()-started:.1f}s)', flush=True)
        db.execute("""WITH RECURSIVE n(i) AS (VALUES(1) UNION ALL SELECT i+1 FROM n WHERE i<1000)
            INSERT OR REPLACE INTO hash_groups SELECT printf('%0128x',i),? FROM n""", (count//1000,))
        db.execute("UPDATE settings SET value='2' WHERE key='hash_groups_ready'")
        db.commit(); db.execute('PRAGMA journal_mode=WAL')
    print(f'Catalog: {catalog.stat().st_size/1024**3:.3f} GiB', flush=True)
    env = dict(os.environ, CCURATE_USAGE_REPEAT='12', SQLITE_TMPDIR=str(root), TMPDIR=str(root))
    for path in ('', 'data/d000005', ''):
        started = time.monotonic()
        run = sp.run([driver, str(destination), path], capture_output=True, text=True, env=env, timeout=1800)
        print(run.stderr, end='', flush=True)
        assert run.returncode == 0, run.stderr
        view = json.loads(run.stdout)
        expected = 1000 if path else count
        assert view['files'] == expected and view['bytes'] == expected*524288, view
        assert view['unique'] == 1000*524288 and view['repeated'] == (expected-1000)*524288, view
        print(f'PASS {path or "/"}: {time.monotonic()-started:.3f}s; {view["files"]:,} files', flush=True)
    print(f'Usage cache: {(destination/".ccurate/usage.sqlite").stat().st_size/1024**2:.3f} MiB', flush=True)
