#!/usr/bin/env python3
"""Real-window regression: saved startup, lazy scope, foreground priority, manual scan."""
from pathlib import Path
import hashlib
import os
import sqlite3
import subprocess as sp
import sys
import tempfile
import time

app = Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory(prefix='ccurate-gui-lazy-') as temporary:
    root = Path(temporary)
    source, destination = root / 'source', root / 'destination'
    source.mkdir(); destination.mkdir()
    (source / 'A').mkdir(); (source / 'B').mkdir()
    (source / 'A/a-small').write_bytes(b'original')
    (source / 'B/other').write_bytes(b'original')
    env = os.environ.copy()
    env.update(DISPLAY='127.0.0.1:197', RAVEN_WINDOW_SYSTEM='x11')
    sp.run([app, '--cli', source, destination, 'scan'], env=env, check=True, capture_output=True)
    dbpath, logpath = destination / '.ccurate/catalog.sqlite', destination / '.ccurate/activity.log'
    def sql(query, args=()):
        with sqlite3.connect(dbpath, timeout=.2) as db:
            return db.execute(query, args).fetchone()
    def events(name):
        return logpath.read_text().count(' | ' + name + ' | ')
    baseline = sql("SELECT value FROM settings WHERE key='scan_started'")
    scan_count = events('scan.start')
    old = hashlib.sha512(b'original').hexdigest()
    (source / 'A/a-small').write_bytes(b'edited while closed')
    (source / 'A/.hidden').write_bytes(b'new hidden file')
    (source / 'B/other').write_bytes(b'unvisited')
    p = None
    with (root / 'x.log').open('w') as xl, (root / 'app.log').open('w') as al:
        x = sp.Popen(['Xvfb', ':197', '-screen', '0', '1600x1000x24', '-nolisten', 'unix',
                      '-nolisten', 'local', '-listen', 'tcp', '-ac'], env=env, stdout=xl, stderr=xl)
        try:
            time.sleep(.5)
            def xd(*args):
                return sp.run(['xdotool', *map(str, args)], env=env, check=True,
                              capture_output=True).stdout.decode().strip()
            def until(check, timeout=30):
                deadline = time.monotonic() + timeout
                while True:
                    assert p.poll() is None, (root / 'app.log').read_text()
                    try:
                        if check():
                            return
                    except (sqlite3.OperationalError, sp.CalledProcessError, FileNotFoundError):
                        pass
                    if time.monotonic() > deadline:
                        raise AssertionError(logpath.read_text()[-6000:])
                    time.sleep(.03)
            def launch():
                global p
                p = sp.Popen([app, source, destination], env=env, stdout=al, stderr=al)
                until(lambda: xd('search', '--onlyvisible', '--name', 'CCurate'))
                win = xd('search', '--onlyvisible', '--name', 'CCurate').splitlines()[-1]
                xd('windowfocus', win)
            def go(path):
                count = events('refresh.finish')
                xd('key', 'ctrl+g'); time.sleep(.12); xd('key', 'ctrl+a')
                if path: xd('type', path)
                else: xd('key', 'BackSpace')
                xd('key', 'Return')
                until(lambda: events('refresh.finish') > count)
                time.sleep(.25)
            def command(text):
                xd('key', 'F9'); time.sleep(.12); xd('type', text); xd('key', 'Return')
            def stop():
                xd('key', 'F10'); p.wait(timeout=20)
                assert p.returncode == 0, (root / 'app.log').read_text()
            def digest(path):
                return sql('SELECT v.hash FROM entries e JOIN versions v ON v.id=e.current '
                           'WHERE e.path=?', (path,))[0]
            launch()
            until(lambda: events('refresh.finish'))
            assert events('scan.start') == scan_count
            assert sql("SELECT value FROM settings WHERE key='scan_started'") == baseline
            assert digest('A/a-small') == old and digest('B/other') == old
            go('A')
            assert digest('A/a-small') == hashlib.sha512(b'edited while closed').hexdigest()
            assert digest('B/other') == old
            assert sql("SELECT 1 FROM entries WHERE path='A/.hidden'") == (1,)
            print('PASS saved startup without tree scan; only entered folder refreshed', flush=True)

            # A large new file keeps background hashing active long enough to
            # verify F5 preemption. Sparse fixture bytes are never copied.
            with (source / 'A/z-large').open('wb') as f:
                f.truncate(64 * 1024**3)
            command('refresh')
            until(lambda: sql("SELECT 1 FROM discoveries d JOIN entries e ON e.id=d.eid "
                              "WHERE e.path='A/z-large'") == (1,))
            time.sleep(.3)
            xd('key', 'Home'); xd('key', 'Down'); xd('key', 'Down')  # parent, .hidden, a-small
            xd('key', 'F5')
            until(lambda: (destination / 'a-small').exists(), timeout=15)
            assert (destination / 'a-small').read_bytes() == b'edited while closed'
            assert not (destination / 'z-large').exists()
            until(lambda: 'copy: completed' in logpath.read_text())
            print('PASS F5 preempts lazy hashing and copies the selected file', flush=True)
            (source / 'A/z-large').unlink()
            go('B')
            assert digest('B/other') == hashlib.sha512(b'unvisited').hexdigest()
            assert events('scan.start') == scan_count
            # Explicit commands still traverse globally and can force SHA-512.
            before = events('scan.finish')
            xd('key', 'ctrl+shift+r')
            until(lambda: events('scan.finish') > before)
            assert 'Full SHA-512 scan started' in logpath.read_text()
            assert sql("SELECT present FROM entries WHERE path='A/z-large'") == (0,)
            print('PASS explicit full SHA-512 scan remains available', flush=True)
            stop()
            # Interrupted baseline stays resumable without an automatic tree scan.
            sql("UPDATE settings SET value='0' WHERE key='scan_complete'")
            before, scan_count = events('refresh.finish'), events('scan.start')
            launch()
            until(lambda: events('refresh.finish') > before)
            assert events('scan.start') == scan_count
            assert sql("SELECT value FROM settings WHERE key='scan_complete'") == ('0',)
            stop()
            print('PASS interrupted session opens its saved catalog without forced rescan', flush=True)
        finally:
            if p and p.poll() is None:
                p.kill(); p.wait(timeout=5)
            x.terminate(); x.wait(timeout=5)
