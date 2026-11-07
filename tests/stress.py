#!/usr/bin/env python3
"""Optional million-record regression; generates only disposable synthetic data.
Usage: python3 tests/stress.py build/ccurate build/workercheck
"""
from pathlib import Path
import hashlib
import sqlite3
import subprocess
import sys
import tempfile
import time

app, worker = (str(Path(x).resolve()) for x in sys.argv[1:3])
for payload in (b'x', b''):
    with tempfile.TemporaryDirectory(prefix='ccurate-stress-') as temp:
        base = Path(temp); src = base/'source'; dst = base/'destination'
        src.mkdir(); dst.mkdir(); (src/'big').mkdir(); (src/'empty').mkdir()
        subprocess.run([app, '--cli', str(src), str(dst), 'scan'], check=True)
        start = time.monotonic()
        with sqlite3.connect(dst/'.ccurate/catalog.sqlite') as db:
            db.execute('PRAGMA journal_mode=OFF'); db.execute('PRAGMA synchronous=OFF')
            db.execute('PRAGMA cache_size=-65536')
            hash_ = hashlib.sha512(payload).hexdigest()
            if payload:
                db.execute('INSERT INTO objects(hash,size) VALUES(?,1)', (hash_,))
            db.execute("""WITH RECURSIVE n(i) AS (VALUES(1) UNION ALL SELECT i+1 FROM n WHERE i<1000000)
                INSERT INTO entries(id,path,parent,name,kind,current)
                SELECT i+100,printf('big/f%07d',i),'big',printf('f%07d',i),'file',i+100 FROM n""")
            db.execute("""INSERT INTO versions(id,eid,mode,size,uid,gid,mtime,ctime,atime,btime,dev,ino,hash)
                SELECT id,id,33188,?,1000,1000,0,0,0,0,1,id,? FROM entries WHERE parent='big'""", (len(payload), hash_))
            db.execute("UPDATE settings SET value='0' WHERE key='hash_groups_ready'")
            db.commit(); db.execute('PRAGMA journal_mode=WAL')
        print(f'Built 1,000,000-entry / identical-hash fixture ({len(payload)} bytes each) in {time.monotonic()-start:.2f}s', flush=True)
        subprocess.run([worker, 'view' if payload else 'empty', str(src), str(dst)], check=True, timeout=90)

with tempfile.TemporaryDirectory(prefix='ccurate-progress-') as temp:
    base = Path(temp); src = base/'source'; dst = base/'destination'
    src.mkdir(); dst.mkdir()
    for n in range(2000): (src/f'item-{n:05d}').write_bytes(b'progress fixture')
    with (src/'large-sparse.bin').open('wb') as f: f.truncate(1024**3)
    subprocess.run([worker, 'scan', str(src), str(dst)], check=True, timeout=90)
    subprocess.run([worker, 'scan', str(src), str(dst)], check=True, timeout=90)
