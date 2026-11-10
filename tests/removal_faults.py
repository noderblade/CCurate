#!/usr/bin/env python3
"""Linux process-kill, cancellation and concurrent-edit tests at actual I/O boundaries."""
from contextlib import closing
from pathlib import Path
import os
import sqlite3
import subprocess as sp
import sys
import tempfile
import time

app = Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory(prefix='ccurate-removal-faults-') as temporary:
    root = Path(temporary)
    hook = root / 'removal_gate.so'
    sp.run(['cc', '-shared', '-fPIC', '-O2', '-Wall', '-Wextra',
            str(Path(__file__).with_name('removal_gate.c')), '-ldl', '-o', hook], check=True)
    for scenario in ('kill-staged', 'kill-unlinked', 'cancel', 'source-edit', 'open-writer',
                     'destination-edit', 'destination-replacement'):
        base = root / scenario
        base.mkdir()
        source, destination = base / 'source', base / 'destination'
        source.mkdir(); destination.mkdir()
        original = source / 'original.txt'
        payload = b'Content surviving interrupted removal.\n'
        original.write_bytes(payload)

        def run(*args, offline=False):
            result = sp.run([app, '--cli', '-' if offline else str(source), destination, *args],
                            capture_output=True, timeout=30)
            assert result.returncode == 0, result.stderr.decode()

        run('scan'); run('copy', 'original.txt', 'Curated')
        target = destination / 'Curated/original.txt'
        wait, ready = base / 'gate', base / 'ready'
        wait.touch()
        point = ('unlinked' if scenario == 'kill-unlinked' else
                 'before-stage' if scenario == 'destination-replacement' else 'staged')
        env = dict(os.environ, CCURATE_TEST_REMOVE_POINT=point,
                   CCURATE_TEST_REMOVE_GATE=str(wait), CCURATE_TEST_REMOVE_READY=str(ready),
                   LD_PRELOAD=':'.join(filter(None, [os.environ.get('LD_PRELOAD'), str(hook)])))
        writer = None
        with (base / 'operation.log').open('wb') as log:
            process = sp.Popen([app, '--cli', source, destination, 'remove',
                                'Curated/original.txt', '--confirm'], env=env, stdout=log, stderr=log)
            try:
                deadline = time.monotonic() + 20
                while not ready.exists():
                    assert process.poll() is None, (base / 'operation.log').read_text()
                    assert time.monotonic() < deadline, scenario
                    time.sleep(.01)
                if scenario.startswith('kill-'):
                    process.kill()
                elif scenario == 'cancel':
                    process.terminate()  # SIGTERM sets the application's cancellation flag.
                elif scenario == 'source-edit':
                    original.write_bytes(b'New source content')
                elif scenario == 'open-writer':
                    writer = original.open('r+b')
                elif scenario == 'destination-edit':
                    staged, = destination.rglob('.ccurate-remove-*')
                    staged.write_bytes(b'New destination content')
                elif scenario == 'destination-replacement':
                    target.rename(base / 'kept-original.txt')
                    target.write_bytes(b'New destination content')
                wait.unlink()
                process.wait(timeout=20)
                assert process.returncode != 0, scenario
            finally:
                if process.poll() is None:
                    process.kill(); process.wait(timeout=10)
                if writer:
                    writer.close()
        run('stats', offline=True)  # Recovery must not require or re-delete source data.
        assert not list(destination.rglob('.ccurate-remove-*')), scenario
        with closing(sqlite3.connect(destination / '.ccurate/catalog.sqlite')) as db:
            assert db.execute("SELECT COUNT(*) FROM operations WHERE kind='remove' AND "
                              "state='pending'").fetchone() == (0,), scenario
            mapping = db.execute('SELECT dest,state FROM objects WHERE size>0').fetchone()
        if scenario == 'kill-unlinked':
            assert not target.exists() and mapping == (None, 'missing'), (scenario, mapping)
        else:
            expected = b'New destination content' if scenario.startswith('destination-') else payload
            assert target.read_bytes() == expected, scenario
            assert mapping == ('Curated/original.txt', 'unchecked'), (scenario, mapping)
        assert original.read_bytes() == (b'New source content' if scenario == 'source-edit' else payload)
        print('PASS ' + scenario, flush=True)
