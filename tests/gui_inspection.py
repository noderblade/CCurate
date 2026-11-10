#!/usr/bin/env python3
"""Real images prove that the active pane supplies bytes, even for identical paths."""
from pathlib import Path
from PIL import Image, ImageGrab
import os
import subprocess as sp
import sys
import tempfile
import time

app = Path(sys.argv[1]).resolve()
screens = Path(sys.argv[2]).resolve() if len(sys.argv) > 2 else None
if screens:
    screens.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(prefix='ccurate-gui-inspection-') as temporary:
    root = Path(temporary)
    source, destination = root / 'source', root / 'destination'
    source.mkdir(); destination.mkdir()
    (source / 'origin').mkdir(); (source / 'duplicates').mkdir(); (destination / 'curated').mkdir()
    for path, color in ((source / 'twin.png', 'red'), (destination / 'twin.png', 'lime'),
                        (source / 'origin/photo.png', 'blue'), (source / 'duplicates/photo.png', 'blue')):
        Image.new('RGB', (320, 200), color).save(path)
    env = dict(os.environ, DISPLAY='127.0.0.1:200', RAVEN_WINDOW_SYSTEM='x11')
    for command in (('scan',), ('copy', 'origin/photo.png', 'curated')):
        sp.run([app, '--cli', source, destination, *command], env=env, check=True, capture_output=True)
    activity = destination / '.ccurate/activity.log'
    with (root / 'x.log').open('w') as xl, (root / 'app.log').open('w') as al:
        x = sp.Popen(['Xvfb', ':200', '-screen', '0', '1920x1200x24', '-nolisten', 'unix',
                      '-nolisten', 'local', '-listen', 'tcp', '-ac'], env=env, stdout=xl, stderr=xl)
        p = None
        try:
            time.sleep(.5)
            p = sp.Popen([app, source, destination], env=env, stdout=al, stderr=al)

            def xd(*args):
                return sp.run(['xdotool', *map(str, args)], env=env, check=True,
                              capture_output=True).stdout.decode().strip()

            def snap(name):
                if screens:
                    ImageGrab.grab(xdisplay=env['DISPLAY']).save(screens / (name + '.png'))

            def until(check):
                end = time.monotonic() + 30
                while time.monotonic() < end:
                    assert p.poll() is None, (root / 'app.log').read_text()
                    try:
                        if check(): return
                    except sp.CalledProcessError: pass
                    time.sleep(.1)
                snap('ERROR')
                raise AssertionError(activity.read_text()[-5000:])

            def keys(*chords):
                for chord in chords:
                    xd('key', chord); time.sleep(.2)

            def preview(color):
                # Only the lower-left preview body; exact solid fixture colors.
                image = ImageGrab.grab(xdisplay=env['DISPLAY']).convert('RGB')
                counts = dict((rgb, n) for n, rgb in image.crop((25, 780, 925, 1120)).getcolors(400000))
                return counts.get(color, 0) > 10000

            def go(path):
                keys('ctrl+g'); xd('type', path); keys('Return', 'End')

            until(lambda: xd('search', '--onlyvisible', '--name', 'manual archive curation'))
            win = xd('search', '--onlyvisible', '--name', 'manual archive curation').splitlines()[-1]
            xd('windowfocus', win); xd('windowsize', win, 1920, 1200)
            until(lambda: 'refresh.finish' in activity.read_text())
            keys('End'); until(lambda: preview((255, 0, 0))); snap('01-source')
            keys('Tab', 'End'); until(lambda: preview((0, 255, 0))); snap('02-destination-unindexed')
            xd('mousemove', 1100, 300); time.sleep(.7)
            assert preview((0, 255, 0)), 'Hover changed inspection context'
            keys('F3', 'ctrl+x')
            until(lambda: 'Hex preview; page/frame 0; destination bytes' in activity.read_text())
            keys('ctrl+x'); until(lambda: preview((0, 255, 0)))
            keys('F3', 'ctrl+d', 'ctrl+alt+Next', 'ctrl+alt+Prior', 'Escape')
            until(lambda: preview((0, 255, 0)))
            keys('ctrl+h'); until(lambda: preview((0, 255, 0)))
            keys('ctrl+h')
            go('curated'); until(lambda: preview((0, 0, 255))); snap('03-destination-provenance')
            keys('F3', 'F3', 'BackSpace', 'End'); until(lambda: preview((0, 255, 0)))
            keys('Tab', 'End'); until(lambda: preview((255, 0, 0)))
            # Mouse focus on a destination header selects that pane, not a source row.
            xd('mousemove', 30, 168); xd('click', 1)
            until(lambda: preview((0, 255, 0)))
            # Repeated cross-pane changes must discard obsolete worker results.
            for _ in range(12): xd('key', 'Tab')
            until(lambda: preview((0, 255, 0)))
            snap('04-destination-after-rapid-switching')
            keys('F10'); p.wait(timeout=20)
            assert p.returncode == 0
            print('PASS source/destination bytes / same paths / hover / F3 / hex / details / history / navigation / mouse / stale results', flush=True)
        finally:
            if p and p.poll() is None:
                p.kill(); p.wait(timeout=10)
            x.terminate(); x.wait(timeout=5)
