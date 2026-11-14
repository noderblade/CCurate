#!/usr/bin/env python3
"""Check native sunburst pixels with TiB-sized branches and byte-sized leaves.

The catalog fixture has five populated levels. No large payloads are created.
Every substantial sector must be uniformly filled, including ring boundaries;
subpixel entries must not paint the bounding rectangles of degenerate triangles.
"""
from contextlib import closing
from pathlib import Path
from PIL import ImageGrab
import math
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

with tempfile.TemporaryDirectory(prefix='ccurate-rings-') as temporary:
    root = Path(temporary)
    source, destination = root / 'source', root / 'destination'
    source.mkdir(); destination.mkdir()
    lengths = {}
    for branch, units in [('Archive', 48), ('Photos', 11), ('Projects', 5), ('Documents', 3)]:
        for child, fraction in enumerate([8, 4, 2, 1, 0, 0, 0, 0]):
            name = f'{branch}/part-{child}/level-3/level-4/payload'
            path = source / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(name)
            lengths[name] = units * fraction * 2**33 if fraction else [1, 7, 1024, 2**20][child-4]
    for i in range(4):
        name = f'tiny-{i}'
        (source / name).write_text(name)
        lengths[name] = i+1
    env = dict(os.environ, DISPLAY='127.0.0.1:241', RAVEN_WINDOW_SYSTEM='x11')
    sp.run([app, '--cli', source, destination, 'scan'], env=env, check=True, capture_output=True)
    database = destination / '.ccurate/catalog.sqlite'
    with closing(sqlite3.connect(database)) as db, db:
        for name, size in lengths.items():
            db.execute('UPDATE versions SET size=? WHERE id=(SELECT current FROM entries WHERE path=?)',
                       (size, name))
        db.execute("INSERT OR REPLACE INTO settings VALUES('hash_groups_ready','0')")
    # A catalog-only fixture must not be replaced by lazy live metadata refresh.
    source.rename(root / 'offline-source')
    log = destination / '.ccurate/activity.log'
    p = None
    with (root / 'x.log').open('w') as xl, (root / 'app.log').open('w') as al:
        x = sp.Popen(['Xvfb', ':241', '-screen', '0', '2800x1800x24', '-nolisten', 'unix',
                      '-nolisten', 'local', '-listen', 'tcp', '-ac'], env=env, stdout=xl, stderr=xl)
        try:
            time.sleep(.5)

            def xd(*args):
                return sp.run(['xdotool', *map(str, args)], env=env, check=True,
                              capture_output=True).stdout.decode().strip()

            def until(check):
                end = time.monotonic() + 30
                while time.monotonic() < end:
                    assert p.poll() is None, (root / 'app.log').read_text()
                    try:
                        if check():
                            return
                    except (sqlite3.OperationalError, sp.CalledProcessError):
                        pass
                    time.sleep(.05)
                raise AssertionError(log.read_text()[-4000:])

            # Fixed-layout fixture; coordinates are only used to locate the
            # chart. Assertions below check the image rather than primitives.
            def circle(width, height, scale):
                w, h = width / scale, height / scale
                top = 258  # The analysis controls fit on one line in these cases.
                upper = (h - 49 - top) * .75 - 12
                chart_x = 24 + (w - 36) * .55
                chart_w = (w - 36) * .45
                return ((chart_x + chart_w/2) * scale,
                        (top + 35 + (upper - 88)/2) * scale,
                        (min(chart_w - 16, upper - 88)/2 - 5) * scale,
                        62 * scale)

            def check_pixels(shot, width, height, scale, depth):
                cx, cy, outer, inner = circle(width, height, scale)
                ring = (outer-inner)/depth
                background = shot.getpixel((round(cx), round(cy + 35*scale)))[:3]

                def pixel(radius, angle):
                    return shot.getpixel((round(cx+math.cos(angle)*radius),
                                          round(cy+math.sin(angle)*radius)))[:3]

                # Each angular region has one flat color per level. Exclude
                # the true boundaries between siblings, not tessellation edges.
                boundaries = [0.0]
                total = sum(lengths.values())
                offset = 0
                for branch in ['Archive', 'Photos', 'Projects', 'Documents']:
                    values = sorted((v for k, v in lengths.items() if k.startswith(branch+'/')), reverse=True)
                    for size in values:
                        offset += size
                        boundaries.append(2*math.pi*offset/total)
                checked = 0
                for degree in range(360):
                    a = math.radians(degree + .37)
                    outside = pixel(outer+3*scale, a-math.pi/2)
                    assert max(abs(outside[c]-background[c]) for c in range(3)) <= 2, (
                        'Rectangle artifact outside the circle', depth, degree, outside, background)
                    if min(abs(a-b) for b in boundaries) * outer < 5*scale:
                        continue
                    angle = a-math.pi/2
                    for level in range(depth):
                        reference = pixel(inner+(level+.5)*ring, angle)
                        assert max(abs(reference[c]-background[c]) for c in range(3)) > 12
                        for fraction in [.12, .27, .73, .88]:
                            color = pixel(inner+(level+fraction)*ring, angle)
                            assert max(abs(color[c]-reference[c]) for c in range(3)) <= 3, (
                                'Nonuniform sector / rectangle artifact', depth, degree, level, color, reference)
                    for level in range(1, depth):
                        color = pixel(inner+level*ring, angle)
                        assert max(abs(color[c]-background[c]) for c in range(3)) > 12, (
                            'Gap between rings', depth, degree, level)
                    checked += 1
                assert checked > 250, checked
                # Alternating shades must make both levels and siblings legible.
                if depth > 1:
                    a, b = pixel(inner+.5*ring, -.9), pixel(inner+1.5*ring, -.9)
                    assert max(abs(a[c]-b[c]) for c in range(3)) > 15, (a, b)
                    a = pixel(inner+1.5*ring, -.9)
                    b = pixel(inner+1.5*ring, 1.4)
                    assert max(abs(a[c]-b[c]) for c in range(3)) > 10, (a, b)

            for theme, scale, width, height, depths in [
                    (7, 1, 1920, 1200, [1, 2, 3, 5]),
                    (7, 1.5, 2800, 1800, [2]),
                    (2, 1, 1920, 1200, [2])]:
                with closing(sqlite3.connect(database)) as db, db:
                    db.executemany('INSERT OR REPLACE INTO settings VALUES(?,?)', [
                        ('ui.theme', str(theme)), ('ui.scale', str(scale)),
                        ('ui.usage', '1 1 1 0 1 0 0 0 0 .55 .25 127\n280 120 88 120 128 94 126\n\n\n')])
                before = log.read_text().count('| usage.view |')
                p = sp.Popen([app, '--browse', destination], env=env, stdout=al, stderr=al)
                until(lambda: xd('search', '--onlyvisible', '--name', 'CCurate'))
                win = xd('search', '--onlyvisible', '--name', 'CCurate').splitlines()[-1]
                xd('windowfocus', win); xd('windowsize', win, width, height)
                xd('mousemove', 4, 4)
                until(lambda: log.read_text().count('| usage.view |') > before)
                current = 1
                for depth in depths:
                    for _ in range(depth-current):
                        before = log.read_text().count('| usage.view |')
                        xd('key', 'bracketright')
                        until(lambda: log.read_text().count('| usage.view |') > before)
                    current = depth
                    time.sleep(.65)
                    shot = ImageGrab.grab(xdisplay=env['DISPLAY'])
                    if screens:
                        shot.crop((0, 0, width, height)).save(screens / f'rings-theme-{theme}-scale-{scale}-depth-{depth}.png')
                    check_pixels(shot, width, height, scale, depth)
                xd('key', 'F10'); p.wait(timeout=20)
                assert p.returncode == 0
            print('PASS: borderless rings, uniform sectors, alternating shades and subpixel leaves at depths 1/2/3/5, 100/150% scale and dark/light themes')
        finally:
            if p and p.poll() is None:
                p.terminate()
                try: p.wait(timeout=5)
                except sp.TimeoutExpired: p.kill(); p.wait()
            x.terminate(); x.wait(timeout=5)
