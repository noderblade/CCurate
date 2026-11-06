#!/usr/bin/env python3
"""Verify global sorting, tie breaks, observations, history and anchors through real view workers."""
from pathlib import Path
import hashlib
import json
import os
import sqlite3
import subprocess as sp
import sys
import tempfile

app, driver = (Path(p).resolve() for p in sys.argv[1:3])
with tempfile.TemporaryDirectory(prefix='ccurate-sort-') as tmp:
    root = Path(tmp); src = root/'source'; dst = root/'destination'
    src.mkdir(); dst.mkdir()
    for base in (src, dst):
        for name in ('a-directory', 'z-directory'): (base/name).mkdir()
        for j in range(2105):
            i = (j * 977) % 2105
            p = base/f'f{i:04}.txt'
            content_id = 10 if 10 <= i < 12 else 12 if 12 <= i < 26 else i
            p.write_bytes(str(content_id).encode().ljust(32 + (content_id*37) % 1091, b'x'))
            p.chmod(0o600 + i % 8)
            os.utime(p, ns=(1_000_000_000, 1_600_000_000_000_000_000 + (i*71 % 2111)*1_000_000))
    sp.run([app, '--cli', src, dst, 'scan'], check=True, capture_output=True)
    with sqlite3.connect(dst/'.ccurate/catalog.sqlite') as db:
        db.execute('UPDATE versions SET btime=(eid*89%997)*1000000,accepted=1')
        for i, state in enumerate(('verified', 'corrupt', 'missing', 'unchecked')):
            name = f'f{i:04}.txt'
            sha = hashlib.sha512((src/name).read_bytes()).hexdigest()
            db.execute('UPDATE objects SET state=?,dest=? WHERE hash=?', (state, name, sha))
        db.execute("UPDATE entries SET discarded=1 WHERE name='f0004.txt'")
        db.execute("UPDATE entries SET error='fixture read error' WHERE name='f0005.txt'")
        # Pending observations must sort by observed values; history must ignore them.
        db.execute("""INSERT INTO discoveries(eid,size,mtime,btime,mode,uid,gid,dev,ino)
                   SELECT id,999999,999999999999999999,0,33279,1000,1000,0,0
                   FROM entries WHERE name='f0006.txt'""")
    def query(side, column, descending, historical=0, page=0, search='', anchor='', fast=0):
        result = sp.run([driver, src, dst, str(side), str(column | (256 if descending else 0)),
                         str(historical), str(page), search, anchor, '0', str(fast)],
                        check=True, capture_output=True, timeout=40)
        return json.loads(result.stdout)
    def status(row, side):
        if row['pending']: return 'ERROR' if row['error'] else 'PENDING'
        if row['rejected']: return 'REJECTED'
        if row['error']: return 'ERROR'
        if row['state']=='corrupt': return 'CHANGED'
        if row['state']=='missing' and row['name'] in ('f0002.txt',): return 'MISSING'
        if row['state']=='unchecked': return 'UNCHECKED'
        if row['state']=='verified' or (not row['hash'] and row['accepted']): return 'OK'
        if row['copies']>1: return '--'
        return 'UNCOPIED' if side else ''
    def key(row, column, side):
        if column==0: return row['name'].encode()
        if column==1: return (status(row,side),row['copies'])
        if column==2: return row['size']
        if column==3: return row['mtime']
        if column==4: return row['btime']
        return row['mode'] & 0o7777
    for side, historical in ((0,0),(1,0),(1,1)):
        for column in range(6):
            for descending in (False,True):
                first=query(side,column,descending,historical)
                rows=first['rows']
                for page in range(1,(first['total']+999)//1000):
                    rows += query(side,column,descending,historical,page)['rows']
                assert len(rows)==first['total'] and len({r['name'] for r in rows})==len(rows)
                groups=[r['kind']=='directory' for r in rows]
                assert groups==sorted(groups,reverse=True)
                files=[r for r in rows if r['kind']!='directory']
                expected=sorted(files,key=lambda r:r['name'].encode())
                expected=sorted(expected,key=lambda r:key(r,column,side),reverse=descending)
                assert files==expected,(side,historical,column,descending)
                anchor=files[1700]['name']
                rank=next(i for i,r in enumerate(rows) if r['name']==anchor)
                anchored=query(side,column,descending,historical,0,anchor=anchor)
                assert anchored['page']==rank//1000 and any(r['name']==anchor for r in anchored['rows'])
                filtered=query(side,column,descending,historical,search='f20')
                assert all('f20' in r['name'] for r in filtered['rows'])
                print(f'PASS side={side} history={historical} column={column} descending={descending}',flush=True)
    live=query(1,2,True,fast=1)['rows'][2]
    assert live['name']=='f0006.txt' and live['size']==999999 and live['pending']
    historical=query(1,2,True,1)['rows']
    assert not any(r['pending'] for r in historical)
    # A live path can change object type while its retained version stays a directory.
    with sqlite3.connect(dst/'.ccurate/catalog.sqlite') as db:
        db.execute("UPDATE entries SET kind='file',current=NULL WHERE name='z-directory'")
    for column in range(6):
        for descending in (False,True):
            rows=query(1,column,descending,1)['rows']
            assert [r['kind'] for r in rows[:2]]==['directory','directory']
            assert all(r['kind']!='directory' for r in rows[2:])
    print('PASS all columns / both directions / both panes / history / global pages / anchors / filters / observations')
