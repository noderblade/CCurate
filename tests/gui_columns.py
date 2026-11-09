#!/usr/bin/env python3
"""Real-window column resizing, viewport auto-fit and session persistence.

Requires Xvfb, xdotool, Pillow and Vulkan. All data are disposable fixtures.
"""
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

with tempfile.TemporaryDirectory(prefix='ccurate-columns-') as temporary:
    root = Path(temporary)
    source, destination = root / 'source', root / 'destination'
    source.mkdir(); destination.mkdir()
    for base in (source, destination):
        (base / 'rows').mkdir()
        for i in range(100):
            name = f'{i:03}-' + ('wide-' * 38 if i == 99 else 'short') + '.txt'
            (base / 'rows' / name).write_text(f'Document {i}\n')
        (base / 'empty').mkdir()
    env = os.environ.copy()
    env.update(DISPLAY='127.0.0.1:198', RAVEN_WINDOW_SYSTEM='x11')
    sp.run([app, '--cli', source, destination, 'scan'], env=env, check=True, capture_output=True)
    database, log = destination / '.ccurate/catalog.sqlite', destination / '.ccurate/activity.log'
    with sqlite3.connect(database) as db:
        db.executemany('INSERT OR REPLACE INTO settings VALUES(?,?)', [
            ('ui.left', 'rows'), ('ui.right', 'rows'),
            ('ui.columns.left', '240 170 90 170 80'),
            ('ui.columns.right', '240 170 90 170 80')])

    def setting(key):
        with sqlite3.connect(database, timeout=.2) as db:
            row = db.execute('SELECT value FROM settings WHERE key=?', (key,)).fetchone()
            return row[0] if row else ''

    def widths(side):
        return list(map(float, setting('ui.columns.' + ('right' if side else 'left')).split()))

    p = None
    with (root / 'x.log').open('w') as xl, (root / 'app.log').open('w') as al:
        x = sp.Popen(['Xvfb', ':198', '-screen', '0', '1920x1200x24', '-nolisten', 'unix',
                      '-nolisten', 'local', '-listen', 'tcp', '-ac'], env=env, stdout=xl, stderr=xl)
        try:
            time.sleep(.5)

            def xd(*args):
                return sp.run(['xdotool', *map(str, args)], env=env, check=True,
                              capture_output=True).stdout.decode().strip()

            def snap(name):
                if screens:
                    ImageGrab.grab(xdisplay=env['DISPLAY']).save(screens / (name + '.png'))

            def until(check):
                deadline = time.monotonic() + 25
                while True:
                    assert p.poll() is None, (root / 'app.log').read_text()
                    try:
                        if check():
                            time.sleep(.15)
                            return
                    except (sqlite3.OperationalError, sp.CalledProcessError, IndexError):
                        pass
                    if time.monotonic() > deadline:
                        snap('ERROR')
                        raise AssertionError(log.read_text()[-5000:])
                    time.sleep(.04)

            def launch():
                global p
                p = sp.Popen([app, source, destination], env=env, stdout=al, stderr=al)
                until(lambda: xd('search', '--onlyvisible', '--name', 'CCurate'))
                win = xd('search', '--onlyvisible', '--name', 'CCurate').splitlines()[-1]
                xd('windowfocus', win); xd('windowsize', win, 1920, 1200)
                time.sleep(.8)

            def click(xpos, ypos, double=False):
                xd('mousemove', round(xpos), round(ypos))
                if double:
                    time.sleep(.45)
                    xd('click', '--repeat', 2, '--delay', 80, 1)
                else:
                    xd('click', 1)
                time.sleep(.2)

            def drag(x0, y0, x1, y1):
                xd('mousemove', round(x0), round(y0)); time.sleep(.45)
                xd('mousedown', 1); xd('mousemove', round(x1), round(y1))
                time.sleep(.15); xd('mouseup', 1); time.sleep(.2)

            def command(text):
                xd('key', 'F9'); time.sleep(.15); xd('type', text); xd('key', 'Return')
                time.sleep(.2)

            def go(path):
                xd('key', 'ctrl+g'); time.sleep(.15); xd('key', 'ctrl+a')
                xd('type', path); xd('key', 'Return'); time.sleep(.6)

            def set_width(column, value):
                xd('key', 'ctrl+alt+c'); time.sleep(.15); xd('key', 'Home')
                for _ in range(column): xd('key', 'Down')
                xd('key', 'Return'); time.sleep(.15); xd('key', 'ctrl+a')
                xd('type', str(value)); xd('key', 'Return'); time.sleep(.2)

            def stop():
                xd('key', 'F10'); p.wait(timeout=20)
                assert p.returncode == 0, (root / 'app.log').read_text()

            launch()
            until(lambda: 'refresh.finish' in log.read_text())
            # All five dividers in each pane resize without sorting or selecting a row.
            for side in (1, 0):
                origin = 974 if side else 20
                click(origin + 20, 253)
                sort_before = setting('ui.sort.' + ('right' if side else 'left'))
                other = widths(1 - side)
                for column in range(5):
                    before = widths(side)
                    edge = origin + sum(before[:column + 1])
                    drag(edge, 200, edge + 20, 200)
                    until(lambda: widths(side)[column] == before[column] + 20)
                    assert setting('ui.sort.' + ('right' if side else 'left')) == sort_before
                assert widths(1 - side) == other
                snap(f'pane-{side}-resized')
                # Clicking the moved header still sorts by the correct column.
                click(origin + sum(widths(side)[:2]) + 20, 200)
                until(lambda: setting('ui.sort.' + ('right' if side else 'left')) == '2')
                xd('key', 'ctrl+F3'); xd('key', 'Home'); time.sleep(.3)
                before = widths(side)
                click(origin + before[0], 200, double=True)
                until(lambda: widths(side)[0] < 240)
                assert widths(side)[1:] == before[1:]
                short_width = widths(side)[0]
                # The off-screen outlier must not affect fitting the initial viewport.
                xd('key', 'End'); time.sleep(.4)
                click(origin + short_width, 200, double=True)
                until(lambda: widths(side)[0] > 1000)
                # A narrower manual value stays in place on subsequent list changes.
                set_width(0, 260)
                until(lambda: widths(side)[0] == 260)
                xd('key', 'Home'); time.sleep(.3)
                assert widths(side)[0] == 260
                print(f'PASS pane {side}: five dividers, sorting, double-click and visible-only fit', flush=True)

            # Last divider remains usable at the end of a horizontally scrolled table.
            xd('key', 'Tab')  # source
            set_width(0, 1200); until(lambda: widths(1)[0] == 1200)
            drag(986, 641, 1884, 641)
            before = widths(1)
            drag(1886, 200, 1846, 200)
            until(lambda: widths(1)[4] == before[4] - 40)
            assert widths(1)[:4] == before[:4]
            # Keyboard auto-fit works even when a header is off-screen.
            xd('key', 'ctrl+alt+w')
            until(lambda: widths(1)[0] < 240)
            assert all(w >= 32 for w in widths(1))
            snap('autofit-visible-source')
            # Empty folders fit the header and parent marker, without an I/O scan for sizing.
            go('empty'); command('autofit')
            until(lambda: widths(1)[0] < 100)
            empty_width = widths(1)
            set_width(0, 'nan'); time.sleep(.2); assert widths(1) == empty_width
            set_width(0, 1); time.sleep(.2); assert widths(1) == empty_width
            set_width(0, 'auto'); time.sleep(.2); assert widths(1) == empty_width
            command('resetcolumns'); until(lambda: widths(1) == [0] * 5)
            go('rows'); set_width(0, 321); until(lambda: widths(1)[0] == 321)
            xd('key', 'ctrl+comma'); time.sleep(.2); xd('key', 'ctrl+a')
            xd('type', '150'); xd('key', 'Return'); time.sleep(.4)
            # Logical widths must not be multiplied again on scale changes or restart.
            assert widths(1)[0] == 321
            saved = widths(0), widths(1)
            snap('columns-at-150-percent')
            stop(); launch()
            assert (widths(0), widths(1)) == saved
            xd('key', 'ctrl+0'); time.sleep(.4)
            # Verify restored geometry, not only the previously written settings.
            drag(974 + 321, 200, 974 + 351, 200)
            until(lambda: widths(1)[0] == 351)
            command('legend'); xd('key', 'Page_Down'); time.sleep(.4)
            snap('directory-status-legend')
            stop()
            print('PASS horizontal scrolling, keyboard widths/fit/reset, empty folders, validation, scale and restart', flush=True)
        finally:
            if p and p.poll() is None:
                p.kill(); p.wait(timeout=5)
            x.terminate(); x.wait(timeout=5)
