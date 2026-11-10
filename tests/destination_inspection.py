#!/usr/bin/env python3
"""Destination metadata and provenance through the real asynchronous view worker."""
from contextlib import closing
from pathlib import Path
import hashlib
import os
import sqlite3
import subprocess as sp
import sys
import tempfile

app, driver = (Path(arg).resolve() for arg in sys.argv[1:3])
with tempfile.TemporaryDirectory(prefix='ccurate-inspection-') as temporary:
    root = Path(temporary)
    source, target = root / 'source', root / 'destination'
    source.mkdir(); target.mkdir(); (target / 'curated').mkdir()
    content = b'Known content with independent destination metadata.\n'
    for i in range(270):
        (source / f'copy-{i:03}.txt').write_bytes(content)
    for name in ('empty-a', 'empty-b'):
        (source / name).touch()

    def cli(*args):
        r = sp.run([app, '--cli', source, target, *args], capture_output=True, timeout=60)
        assert r.returncode == 0, r.stderr.decode(errors='replace')

    cli('scan')
    for name in ('copy-000.txt', 'empty-a', 'empty-b'):
        cli('copy', name, 'curated')
    cli('move', 'curated/copy-000.txt', 'curated/archive.txt')
    cli('reject', 'copy-269.txt')
    cli('review', 'copy-001.txt')
    # Historical source metadata differs from the current destination metadata.
    os.utime(target / 'curated/archive.txt', (946684800, 946684800))
    if os.name != 'nt':
        (target / 'curated/archive.txt').chmod(0o640)
    (target / 'copy-000.txt').write_bytes(b'Different unindexed destination bytes')
    database = target / '.ccurate/catalog.sqlite'

    def snapshot():
        with closing(sqlite3.connect(database)) as db:
            return {table: db.execute(f'SELECT * FROM {table}').fetchall()
                    for table in ('entries', 'versions', 'objects', 'empty_copies', 'events')}

    before = snapshot()
    source.rename(root / 'disconnected-source')

    def inspect(path, page=0):
        r = sp.run([driver, target, path, str(page)], capture_output=True, timeout=20)
        assert r.returncode == 0, r.stderr.decode(errors='replace')
        return r.stdout.decode()

    detail = inspect('curated/archive.txt')
    assert 'DESTINATION\n' in detail and 'curated/archive.txt' in detail
    assert 'INDEXED COPY [verified]' in detail and 'Last verification:' in detail
    assert 'Copied from: copy-000.txt' in detail
    assert '2000-01-01' in detail and f'({len(content)} B)' in detail
    if os.name != 'nt':
        assert 'mode 0640' in detail
    digest = hashlib.sha512(content).hexdigest()
    assert digest[:64] + '\n' + digest[64:] in detail
    assert 'copy-001.txt' in detail and '[reviewed]' in detail
    assert detail.count('(v') == 256 and 'More occurrences on the next page.' in detail
    last = inspect('curated/archive.txt', 1)
    assert last.count('(v') == 14 and 'copy-269.txt' in last and '[rejected]' in last
    assert 'No occurrences on this page.' in inspect('curated/archive.txt', 2)
    assert 'page 1;' in inspect('curated/archive.txt', -1)
    print('PASS destination metadata / indexed digest / paged provenance / offline source')

    empty = inspect('curated/empty-a')
    assert 'EMPTY FILE: tracked individually' in empty
    assert 'empty-a' in empty and 'empty-b' not in empty and 'SOURCE CONTENT' not in empty
    assert 'NOT INDEXED' in inspect('copy-000.txt')
    assert 'Directory size is filesystem metadata' in inspect('curated')
    assert 'SESSION DATA' in inspect('.ccurate/activity.log')
    assert 'Select an entry in the destination pane.' == inspect('../escape')
    print('PASS empty-file independence / unindexed file / directory / session data / path guard')

    original = target / 'curated/archive.txt'
    original.write_bytes(b'Altered size')
    assert 'WARNING: destination type or size differs' in inspect('curated/archive.txt')
    original.unlink()
    missing = inspect('curated/archive.txt')
    assert 'Cannot read destination metadata' in missing and 'INDEXED COPY' in missing
    if os.name != 'nt':
        original.symlink_to(root / 'disconnected-source/copy-000.txt')
        assert 'Symlink: content is not followed' in inspect('curated/archive.txt')
        (target / 'escape').symlink_to(root / 'disconnected-source', target_is_directory=True)
        assert 'Cannot read destination metadata' in inspect('escape/copy-000.txt')
    assert snapshot() == before, 'Inspection changed catalog data'
    print('PASS changed/missing destination / no symlink traversal / catalog unchanged')
