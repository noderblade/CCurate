#!/usr/bin/env python3
"""Real-window removal confirmation, keyboard/menu access and recopy regression."""
from contextlib import closing
from pathlib import Path
from PIL import ImageGrab
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

with tempfile.TemporaryDirectory(prefix='ccurate-gui-remove-') as temporary:
    root = Path(temporary)
    source, destination = root / 'source', root / 'destination'
    source.mkdir(); destination.mkdir()
    for name in ('document.txt', 'duplicate.txt'):
        (source / name).write_text('A document worth preserving.\n')
    env = dict(os.environ, DISPLAY='127.0.0.1:199', RAVEN_WINDOW_SYSTEM='x11')
    for command in (('scan',), ('copy', 'document.txt', '')):
        sp.run([app, '--cli', source, destination, *command], env=env, check=True,
               capture_output=True)
    database = destination / '.ccurate/catalog.sqlite'
    activity = destination / '.ccurate/activity.log'
    target = destination / 'document.txt'

    def sql(query):
        with closing(sqlite3.connect(database, timeout=.2)) as db:
            return db.execute(query).fetchone()

    def snap(name):
        if screens:
            ImageGrab.grab(xdisplay=env['DISPLAY']).save(screens / (name + '.png'))

    with (root / 'x.log').open('w') as xl, (root / 'app.log').open('w') as al:
        x = sp.Popen(['Xvfb', ':199', '-screen', '0', '1920x1200x24', '-nolisten', 'unix',
                      '-nolisten', 'local', '-listen', 'tcp', '-ac'], env=env, stdout=xl, stderr=xl)
        p = None
        try:
            time.sleep(.5)
            p = sp.Popen([app, source, destination], env=env, stdout=al, stderr=al)

            def xd(*args):
                return sp.run(['xdotool', *map(str, args)], env=env, check=True,
                              capture_output=True).stdout.decode().strip()

            def until(check):
                deadline = time.monotonic() + 30
                while time.monotonic() < deadline:
                    assert p.poll() is None, (root / 'app.log').read_text()
                    try:
                        if check():
                            time.sleep(.2)
                            return
                    except (sqlite3.Error, sp.CalledProcessError):
                        pass
                    time.sleep(.05)
                snap('ERROR')
                raise AssertionError(activity.read_text()[-6000:])

            def keys(*chords):
                for chord in chords:
                    xd('key', chord); time.sleep(.2)

            def click(xpos, ypos):
                xd('mousemove', xpos, ypos); xd('click', 1); time.sleep(.3)

            until(lambda: xd('search', '--onlyvisible', '--name', 'manual archive curation'))
            win = xd('search', '--onlyvisible', '--name', 'manual archive curation').splitlines()[-1]
            xd('windowfocus', win); xd('windowsize', win, 1920, 1200)
            until(lambda: 'refresh.finish' in activity.read_text())
            keys('ctrl+Delete')  # Source focus: there is no deletion action.
            assert target.exists()
            assert sql("SELECT COUNT(*) FROM operations WHERE kind='remove'") == (0,)
            keys('Tab', 'Home', 'Down', 'ctrl+Delete')
            snap('01-remove-confirmation')
            keys('Return')  # Cancel is the default, including when Enter is pressed.
            assert target.exists()
            assert sql("SELECT COUNT(*) FROM operations WHERE kind='remove'") == (0,)
            # At maximum scale, the message scrolls and confirmation stays usable.
            keys('F9'); xd('type', 'scale 400'); keys('Return', 'ctrl+Delete')
            snap('01b-confirmation-scale-400')
            keys('End'); snap('01c-confirmation-scrolled'); keys('Return')
            assert target.exists()
            keys('F9'); xd('type', 'scale 100'); keys('Return')
            keys('ctrl+Delete', 'Tab', 'Return')
            until(lambda: sql('SELECT dest,state FROM objects WHERE size>0') == (None, 'missing'))
            assert not target.exists()
            snap('02-all-occurrences-unsecured')
            assert sql('SELECT COUNT(*) FROM versions WHERE accepted=1 AND size>0 AND hash IS NOT NULL') == (2,)
            keys('F5')
            until(lambda: target.exists() and sql("SELECT state FROM objects WHERE size>0") == ('verified',))
            print('PASS source guard / default Cancel / explicit keyboard removal / F5 recopy', flush=True)

            # Files menu (last action): mouse can reach the same confirmation.
            keys('Home', 'Down')
            click(365, 1180)
            row = 1200 - 42 - (42 + 9 * 29 + 100) + 40 + 8 * 29 + 14
            xd('mousemove', 280, row); time.sleep(.5)
            snap('03-menu-hover-help')
            click(280, row)
            snap('04-menu-confirmation')
            click(800, 737)  # Mouse Cancel.
            assert target.exists()
            keys('F9'); xd('type', 'remove'); keys('Return')
            snap('05-command-confirmation')
            click(650, 737)  # Mouse Remove copy.
            until(lambda: not target.exists())
            until(lambda: sql("SELECT COUNT(*) FROM events WHERE op='remove'") == (2,))
            assert all((source / name).read_text() == 'A document worth preserving.\n'
                       for name in ('document.txt', 'duplicate.txt'))
            keys('F1'); snap('06-help'); keys('F1', 'F10')
            p.wait(timeout=20)
            assert p.returncode == 0, (root / 'app.log').read_text()
            print('PASS menu tooltip / mouse Cancel and Remove / F9 / help / source preservation', flush=True)
        finally:
            if p and p.poll() is None:
                p.kill(); p.wait(timeout=10)
            x.terminate(); x.wait(timeout=5)
