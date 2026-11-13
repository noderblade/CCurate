#!/usr/bin/env python3
"""Native UI regression: tab isolation, tree, hover, charts, columns and reveal."""
from contextlib import closing
from pathlib import Path
from PIL import ImageGrab
import base64
import os
import sqlite3
import subprocess as sp
import sys
import tempfile
import time

app = Path(sys.argv[1]).resolve()
screens = Path(sys.argv[2]).resolve() if len(sys.argv) > 2 else None
if screens:
    screens.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(prefix='ccurate-gui-usage-') as temporary:
    root = Path(temporary)
    source, destination = root / 'source', root / 'destination'
    source.mkdir(); destination.mkdir(); (destination / 'curated').mkdir()
    for name, data in [('Photos/2023/photo.bin', b'a' * 40000),
                       ('Photos/2024/photo.bin', b'b' * 30000),
                       ('Backups/photo-copy.bin', b'a' * 40000),
                       ('Projects/board.bin', b'c' * 20000),
                       ('.empty', b''), ('notes.txt', b'logical usage fixture')]:
        path = source / name; path.parent.mkdir(parents=True, exist_ok=True); path.write_bytes(data)
    env = dict(os.environ, DISPLAY='127.0.0.1:240', RAVEN_WINDOW_SYSTEM='x11')
    sp.run([app, '--cli', source, destination, 'scan'], env=env, check=True, capture_output=True)
    database = destination / '.ccurate/catalog.sqlite'
    with closing(sqlite3.connect(database)) as db, db:
        db.execute("INSERT OR REPLACE INTO settings VALUES('ui.left','curated')")
    log = destination / '.ccurate/activity.log'
    p = None
    with (root / 'x.log').open('w') as xl, (root / 'app.log').open('w') as al:
        x = sp.Popen(['Xvfb', ':240', '-screen', '0', '1920x1200x24', '-nolisten', 'unix',
                      '-nolisten', 'local', '-listen', 'tcp', '-ac'], env=env, stdout=xl, stderr=xl)
        try:
            time.sleep(.5)

            def xd(*args):
                return sp.run(['xdotool', *map(str, args)], env=env, check=True,
                              capture_output=True).stdout.decode().strip()

            def setting(key):
                with closing(sqlite3.connect(database, timeout=.2)) as db:
                    row = db.execute('SELECT value FROM settings WHERE key=?', (key,)).fetchone()
                    return row[0] if row else ''

            def preferences():
                lines = setting('ui.usage').split('\n')
                if len(lines) < 4:
                    return None
                return lines[0].split(), list(map(float, lines[1].split())), base64.b64decode(lines[2]).decode(), base64.b64decode(lines[3]).decode()

            def snap(name):
                if screens:
                    ImageGrab.grab(xdisplay=env['DISPLAY']).save(screens / (name + '.png'))

            def until(check):
                end = time.monotonic() + 30
                while time.monotonic() < end:
                    assert p.poll() is None, (root / 'app.log').read_text()
                    try:
                        if check():
                            time.sleep(.1)
                            return
                    except (sqlite3.OperationalError, sp.CalledProcessError, TypeError, IndexError):
                        pass
                    time.sleep(.04)
                snap('ERROR')
                raise AssertionError(log.read_text()[-7000:] + '\n' + (root / 'app.log').read_text()[-3000:])

            def keys(*chords):
                for chord in chords:
                    xd('key', chord); time.sleep(.15)

            def views():
                return [line for line in log.read_text().splitlines() if '| usage.view |' in line]

            def launch():
                global p
                p = sp.Popen([app, source, destination], env=env, stdout=al, stderr=al)
                until(lambda: xd('search', '--onlyvisible', '--name', 'CCurate'))
                win = xd('search', '--onlyvisible', '--name', 'CCurate').splitlines()[-1]
                xd('windowfocus', win); xd('windowsize', win, 1920, 1200); time.sleep(.6)

            launch()
            keys('ctrl+Tab')
            until(lambda: views() and 'Logical=130021 unique=90021 repeated=40000' in views()[-1])
            snap('01-rings-root')
            keys('F5', 'F8')
            assert not (destination / 'Photos').exists(), 'Hidden Curation selection was copied'
            with closing(sqlite3.connect(database)) as db:
                assert db.execute('SELECT count(*) FROM entries WHERE discarded=1').fetchone()[0] == 0
            before = preferences()[3]
            xd('mousemove', 90, 330); time.sleep(.8)
            assert preferences()[3] == before, 'Hover selected a file'
            keys('Down')
            until(lambda: preferences()[3] == 'Photos')
            keys('Return')
            until(lambda: 'path="Photos"' in views()[-1])
            snap('02-subdirectory')
            # Root breadcrumb is clickable, as are intermediate ancestors.
            xd('mousemove', 45, 160); xd('click', 1)
            until(lambda: preferences()[2] == '' and preferences()[3] == 'Photos')
            keys('Return')
            until(lambda: 'path="Photos"' in views()[-1])
            keys('BackSpace')
            until(lambda: preferences()[2] == '' and preferences()[3] == 'Photos' and 'path=""' in views()[-1])
            keys('space')
            until(lambda: '; 7 rows;' in views()[-1])
            snap('03-expanded-tree')
            keys('ctrl+alt+c', 'Down', 'space', 'Escape')
            until(lambda: int(preferences()[0][11]) == 125)
            keys('ctrl+alt+w')
            until(lambda: preferences()[1][0] < 280)
            width = preferences()[1][0]
            xd('mousemove', round(20+width), 302); time.sleep(.45)
            xd('mousedown', 1); xd('mousemove', round(60+width), 302); xd('mouseup', 1)
            until(lambda: preferences()[1][0] > width+30)
            keys('t')
            until(lambda: preferences()[0][5] == '1')
            snap('04-treemap')
            keys('r', 'bracketright', 'bracketright')
            until(lambda: preferences()[0][4] == '5' and preferences()[0][5] == '0')
            keys('ctrl+f')
            until(lambda: preferences()[0][3] == '1')
            keys('F1')
            snap('05-help')
            keys('Escape', 'F3')
            until(lambda: preferences()[0][0] == '0')
            assert setting('ui.left') == 'curated', 'Reveal changed destination context'
            snap('06-reveal-curation')
            keys('ctrl+Tab')
            until(lambda: preferences()[0][0] == '1')
            saved = preferences()
            keys('F10'); p.wait(timeout=20)
            assert p.returncode == 0, (root / 'app.log').read_text()
            launch()
            until(lambda: len(views()) > 2 and preferences()[0][0] == '1')
            assert preferences()[2:] == saved[2:]
            assert preferences()[0][3:6] == saved[0][3:6]
            snap('07-restored-session')
            keys('F10'); p.wait(timeout=20)
            assert p.returncode == 0
            print('PASS: native usage tabs, non-selecting hover, tree, navigation focus, chart modes, columns, help, reveal and session restoration')
        finally:
            if p and p.poll() is None:
                p.terminate()
                try: p.wait(timeout=5)
                except sp.TimeoutExpired: p.kill(); p.wait()
            x.terminate(); x.wait(timeout=5)
