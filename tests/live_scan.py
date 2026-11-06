#!/usr/bin/env python3
"""Live inventory / cancelled rescan regression using real disposable files.
Usage: python3 tests/live_scan.py build/ccurate build/workercheck
"""
from pathlib import Path
import hashlib, json, sqlite3, subprocess as sp, sys, tempfile
app, worker = (str(Path(a).resolve()) for a in sys.argv[1:3])
with tempfile.TemporaryDirectory(prefix='ccurate-live-') as t:
    root=Path(t); src=root/'source'; dst=root/'destination'
    src.mkdir(); dst.mkdir()
    original=b'archived original\n'
    (src/'a-kept.txt').write_bytes(original)
    def run(*args):
        sp.run([app,'--cli',str(src),str(dst),*args],check=True,capture_output=True,timeout=90)
    run('scan'); run('copy','a-kept.txt','Curated')
    (src/'a-kept.txt').write_bytes(b'changed source contents!!\n')
    assert (src/'a-kept.txt').stat().st_size==26
    for i in range(20000): (src/f'new-{i:05}.txt').write_bytes(b'new data')
    sp.run([worker,'live',str(src),str(dst)],check=True,timeout=30)
    with sqlite3.connect(dst/'.ccurate/catalog.sqlite') as db:
        assert db.execute("SELECT value FROM settings WHERE key='scan_complete'").fetchone()==('0',)
        assert db.execute('SELECT count(*) FROM discoveries').fetchone()[0]>0
    archive=[json.loads(s) for s in (dst/'.ccurate/archive.jsonl').read_text().splitlines()]
    assert any(s.get('type')=='entry' and s.get('path')=='a-kept.txt' for s in archive)
    assert not (dst/'Pending').exists()
    assert (dst/'Curated/a-kept.txt').read_bytes()==original
    run('scan'); run('audit')
    with sqlite3.connect(dst/'.ccurate/catalog.sqlite') as db:
        assert db.execute('SELECT count(*) FROM discoveries').fetchone()==(0,)
        assert db.execute("SELECT value FROM settings WHERE key='scan_complete'").fetchone()==('1',)
        versions=db.execute("SELECT v.hash,v.accepted FROM versions v JOIN entries e ON e.id=v.eid WHERE e.path='a-kept.txt' ORDER BY v.id").fetchall()
        assert versions==[(hashlib.sha512(original).hexdigest(),1),(hashlib.sha512((src/'a-kept.txt').read_bytes()).hexdigest(),0)]
    print('restart / completion / old retained version / destination audit PASS')
