#!/usr/bin/env python3
"""Choose roots by browsing in real windows, without typing any paths.

Requires Xvfb, xdotool, Pillow and Vulkan. All data and sessions are disposable.
"""
from pathlib import Path
from PIL import ImageGrab
import fcntl
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
with tempfile.TemporaryDirectory(prefix='ccurate-startup-') as temporary:
    root = Path(temporary)
    source, destination, hidden = (root / name for name in ('a-source', 'b-destination', '.hidden-source'))
    for directory in (source, destination, hidden):
        directory.mkdir()
        if directory != destination:
            (directory / 'hello.txt').write_text('Folder chooser fixture.\n')
    env = os.environ.copy()
    env.update(DISPLAY='127.0.0.1:196', RAVEN_WINDOW_SYSTEM='x11')
    dbpath = destination / '.ccurate/catalog.sqlite'
    logpath = destination / '.ccurate/activity.log'
    with (root / 'x.log').open('w') as xl, (root / 'app.log').open('w') as al:
        x = sp.Popen(['Xvfb', ':196', '-screen', '0', '1280x900x24', '-nolisten', 'unix',
                      '-nolisten', 'local', '-listen', 'tcp', '-ac'], env=env, stdout=xl, stderr=xl)
        p = None
        try:
            def xd(*args):
                return sp.run(['xdotool', *map(str, args)], env=env, check=True,
                              capture_output=True).stdout.decode().strip()

            def snapshot(name):
                if screens:
                    ImageGrab.grab(xdisplay=env['DISPLAY']).save(screens / (name + '.png'))

            def until(check):
                deadline = time.monotonic() + 30
                while True:
                    assert p.poll() is None, (root / 'app.log').read_text()
                    try:
                        if check():
                            break
                    except (FileNotFoundError, sqlite3.OperationalError, sp.CalledProcessError):
                        pass
                    if time.monotonic() >= deadline:
                        snapshot('ERROR')
                        raise AssertionError((root / 'app.log').read_text())
                    time.sleep(.05)
                time.sleep(.3)

            def key(value):
                xd('key', value)
                time.sleep(.2)

            def click(xpos, ypos):
                xd('mousemove', xpos, ypos)
                xd('click', 1)
                time.sleep(.3)

            def launch(*args):
                global p
                p = sp.Popen([app, *args], cwd=root, env=env, stdout=al, stderr=al)
                until(lambda: xd('search', '--onlyvisible', '--name', 'CCurate - choose folders'))
                win = xd('search', '--onlyvisible', '--name', 'CCurate - choose folders').splitlines()[-1]
                xd('windowfocus', win)
                time.sleep(.5)

            def stop(keyname='Escape'):
                key(keyname)
                p.wait(timeout=20)
                assert p.returncode == 0, (root / 'app.log').read_text()

            def choose(side, row):
                key('alt+d' if side else 'alt+s')
                time.sleep(.3)
                key('Home')
                for _ in range(row):
                    key('Down')
                # From the folder list, reverse Tab focuses the Select folder button.
                key('shift+Tab')
                key('Return')

            def source_setting():
                with sqlite3.connect(dbpath) as db:
                    return db.execute("SELECT value FROM settings WHERE key='source'").fetchone()[0]

            def workspace():
                until(lambda: xd('search', '--onlyvisible', '--name', 'manual archive curation'))
                win = xd('search', '--onlyvisible', '--name', 'manual archive curation').splitlines()[-1]
                xd('windowfocus', win)

            time.sleep(.5)
            launch()
            snapshot('01-empty-chooser')
            key('ctrl+Return')  # Missing choices must not start a catalog.
            assert not dbpath.exists()
            key('alt+s')
            snapshot('02-folder-browser')
            key('Escape')
            stop()
            assert not list(root.rglob('.ccurate'))
            print('PASS cancel / missing selection / no session side effects', flush=True)

            launch()
            choose(0, 1)
            snapshot('03-source-selected')
            choose(1, 1)  # Same source/destination must be refused before any write.
            key('ctrl+Return')
            snapshot('04-invalid-pair')
            assert not list(root.rglob('.ccurate'))
            key('alt+d')
            key('BackSpace')  # The picker reopens in the previously chosen source.
            key('Home')
            key('Down')
            key('Down')
            key('shift+Tab')
            key('Return')
            key('ctrl+Return')
            workspace()
            until(lambda: logpath.exists() and 'operation.finish' in logpath.read_text())
            assert source_setting() == str(source)
            snapshot('05-workspace')
            stop('F10')
            assert not (source / '.ccurate').exists()
            print('PASS keyboard-only browsing / equal-root rejection / scan after start', flush=True)

            launch(source)
            # The explicit source is prefilled; choose the destination using clicks only.
            click(820, 253)
            click(350, 218)
            click(820, 557)
            click(140, 441)
            workspace()
            assert source_setting() == str(source)
            stop('F10')
            print('PASS prefilled source / mouse-only destination selection / session resume', flush=True)

            launch('--browse')
            choose(1, 2)
            key('ctrl+Return')
            workspace()
            snapshot('06-offline-archive')
            stop('F10')
            print('PASS offline destination browser', flush=True)

            lock = (destination / '.ccurate/session.lock').open('r+')
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            launch(source)
            choose(1, 2)
            key('ctrl+Return')
            until(lambda: 'already has an active session' in (root / 'app.log').read_text())
            until(lambda: xd('search', '--onlyvisible', '--name', 'CCurate - choose folders'))
            win = xd('search', '--onlyvisible', '--name', 'CCurate - choose folders').splitlines()[-1]
            xd('windowfocus', win)
            snapshot('07-catalog-error')
            stop()
            lock.close()
            print('PASS catalog error shown in chooser / cancellation after failure', flush=True)

            # Hidden roots are visible and selectable without a path input.
            launch()
            choose(0, 0)
            snapshot('08-hidden-root-selected')
            stop()
            assert not (hidden / '.ccurate').exists()
            print('PASS hidden folder browsing', flush=True)
        finally:
            if p and p.poll() is None:
                p.kill()
                p.wait(timeout=5)
            x.terminate()
            x.wait(timeout=5)
