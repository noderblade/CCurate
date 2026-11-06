#!/usr/bin/env python3
"""Native font chooser regression with an isolated Fontconfig font directory.

Requires Xvfb, xdotool, Pillow, fontTools, DejaVu/URW fonts and Vulkan.
Fixtures include a TTC whose first face is proportional and second is monospace.
Only disposable fixture fonts are copied/renamed/removed.
"""
from pathlib import Path
from PIL import ImageGrab
from fontTools.ttLib import TTFont, TTCollection
import hashlib
import os
import shutil
import sqlite3
import subprocess as sp
import sys
import tempfile
import time

app = Path(sys.argv[1]).resolve()
screens = Path(sys.argv[2]).resolve() if len(sys.argv) > 2 else None
if screens:
    screens.mkdir(parents=True, exist_ok=True)
fonts = Path('/usr/share/fonts/truetype/dejavu')
with tempfile.TemporaryDirectory(prefix='ccurate-fonts-') as temp:
    root = Path(temp)
    src, dst, fontdir = [root / name for name in ('source', 'destination', 'fonts')]
    for directory in (src, dst, fontdir):
        directory.mkdir()
    (src / 'a-file.txt').write_text('Monospace UI fixture: Il1 O0o.\n')
    regular = fontdir / 'mono.ttf'
    shutil.copyfile(fonts / 'DejaVuSansMono.ttf', regular)
    shutil.copyfile(fonts / 'DejaVuSansMono-Bold.ttf', fontdir / 'bold.ttf')
    otf = fontdir / 'nimbus.otf'
    shutil.copyfile('/usr/share/fonts/opentype/urw-base35/NimbusMonoPS-Regular.otf', otf)
    # Give the second TTC face a unique family name for unambiguous selection.
    mono = TTFont(regular)
    for record in mono['name'].names:
        if record.nameID in (1, 4, 6, 16):
            value = 'CollectionMono' if record.nameID == 6 else 'Collection Mono'
            record.string = value.encode(record.getEncoding())
    collection = TTCollection()
    collection.fonts = [TTFont(fonts / 'DejaVuSans.ttf'), mono]
    ttc = fontdir / 'collection.ttc'
    collection.save(ttc)
    collection.close()
    ttc_hash = hashlib.sha512(ttc.read_bytes()).hexdigest()
    config = root / 'fonts.conf'
    config.write_text(f'''<?xml version="1.0"?>
<!DOCTYPE fontconfig SYSTEM "urn:fontconfig:fonts.dtd">
<fontconfig><dir>{fontdir}</dir><cachedir>{root / 'font-cache'}</cachedir>
<alias><family>monospace</family><prefer><family>DejaVu Sans Mono</family></prefer></alias>
</fontconfig>''')
    env = os.environ.copy()
    env.update(DISPLAY='127.0.0.1:192', RAVEN_WINDOW_SYSTEM='x11', FONTCONFIG_FILE=str(config))
    dbpath = dst / '.ccurate/catalog.sqlite'
    log = dst / '.ccurate/activity.log'
    with (root / 'x.log').open('w') as xl, (root / 'app.log').open('w') as al:
        x = sp.Popen(['Xvfb', ':192', '-screen', '0', '1920x1200x24', '-nolisten', 'unix',
                      '-nolisten', 'local', '-listen', 'tcp', '-ac'], env=env, stdout=xl, stderr=xl)
        p = None
        try:
            def xd(*args):
                return sp.run(['xdotool', *map(str, args)], env=env, check=True,
                              capture_output=True).stdout.decode().strip()

            def snap(name):
                image = ImageGrab.grab(xdisplay=env['DISPLAY'])
                if screens:
                    image.save(screens / (name + '.png'))
                print(name, flush=True)
                return image

            def until(check, timeout=20):
                end = time.monotonic() + timeout
                while True:
                    assert p.poll() is None, (root / 'app.log').read_text()
                    try:
                        if check():
                            break
                    except (sqlite3.OperationalError, FileNotFoundError, sp.CalledProcessError):
                        pass
                    if time.monotonic() > end:
                        snap('ERROR-timeout')
                        raise AssertionError((root / 'app.log').read_text() + '\n' + log.read_text())
                    time.sleep(.05)
                time.sleep(.2)

            def setting(key):
                with sqlite3.connect(dbpath, timeout=.2) as db:
                    row = db.execute('SELECT value FROM settings WHERE key=?', (key,)).fetchone()
                    return row[0] if row else None

            def click(x, y):
                xd('mousemove', x, y)
                xd('click', 1)
                time.sleep(.2)

            def key(*keys):
                xd('key', *keys)
                time.sleep(.15)

            def choose_filter(value):
                key('ctrl+alt+f')
                xd('type', '--clearmodifiers', value)
                time.sleep(.4)

            def start(*args):
                global p
                discoveries = log.read_text().count('fonts.discovered') if log.exists() else 0
                p = sp.Popen([app, *args], env=env, stdout=al, stderr=al)
                until(lambda: xd('search', '--name', 'CCurate'))
                win = xd('search', '--name', 'CCurate').splitlines()[-1]
                xd('windowfocus', win)
                xd('windowsize', win, 1920, 1200)
                until(lambda: log.read_text().count('fonts.discovered') > discoveries)
                time.sleep(.5)

            def stop():
                key('F10')
                p.wait(timeout=20)
                assert p.returncode == 0, (root / 'app.log').read_text()

            time.sleep(.5)
            start(src, dst)
            until(lambda: 'operation.finish' in log.read_text())
            assert '4 compatible monospace faces' in log.read_text(), log.read_text()
            # Open Layout > Monospace font entirely with the mouse.
            click(1552, 1180)
            click(1278, 1044)
            snap('01-font-dialog')
            # Sorted rows: automatic, bundled, Collection Mono, DejaVu Bold, Book.
            click(700, 478)
            snap('02-system-font-preview')
            click(610, 904)
            until(lambda: setting('ui.font.file') == str(regular))
            assert setting('ui.font.family') == 'DejaVu Sans Mono'
            key('F7')
            snap('03-monospace-native-dialog')
            key('Escape')
            # Cancel a different sample; persisted selection remains unchanged.
            choose_filter('Collection Mono')
            snap('04-collection-face-preview')
            key('Escape')
            assert setting('ui.font.file') == str(regular)
            choose_filter('Collection Mono')
            key('Return')
            until(lambda: setting('ui.font.file') == str(ttc))
            assert setting('ui.font.index') == '1'
            assert hashlib.sha512(ttc.read_bytes()).hexdigest() == ttc_hash
            snap('05-collection-face-applied')
            choose_filter('Nimbus Mono PS / Regular')
            key('Return')
            until(lambda: setting('ui.font.file') == str(otf))
            key('F7')
            snap('05-opentype-native-dialog')
            key('Escape')
            # A non-monospace family is excluded; clearing the filter restores rows.
            choose_filter('DejaVu Sans /')
            snap('06-no-proportional-fonts')
            key('Return')
            assert setting('ui.font.file') == str(otf)
            key('ctrl+u', 'Home', 'Down', 'Tab', 'Return')
            until(lambda: setting('ui.font.file') == '@bundled')
            # A vanished file after discovery is an error, never a silent Apply.
            choose_filter('DejaVu Sans Mono / Book')
            key('Escape')
            hidden = root / 'hidden.ttf'
            regular.rename(hidden)
            choose_filter('DejaVu Sans Mono / Book')
            snap('07-font-read-error')
            key('Return')
            assert setting('ui.font.file') == '@bundled'
            until(lambda: 'font.read' in log.read_text())
            key('Escape')
            hidden.rename(regular)
            choose_filter('DejaVu Sans Mono / Book')
            key('Return')
            until(lambda: setting('ui.font.file') == str(regular))
            # Refresh works in place; F5 here must not copy the selected source.
            key('ctrl+alt+f')
            n = log.read_text().count('fonts.discovered')
            key('F5')
            until(lambda: log.read_text().count('fonts.discovered') > n)
            assert not (dst / 'a-file.txt').exists()
            key('Escape')
            # At 400%, the list scrolls and all controls remain keyboard-accessible.
            key('ctrl+comma', 'ctrl+a')
            xd('type', '400')
            key('Return', 'ctrl+alt+f', 'End')
            snap('08-font-dialog-at-400')
            key('Return')
            until(lambda: setting('ui.font.file') == str(otf))
            key('ctrl+0')
            choose_filter('DejaVu Sans Mono / Book')
            key('Return')
            until(lambda: setting('ui.font.file') == str(regular))
            stop()
            # Saved family/style recovers after its installed file is moved.
            moved = fontdir / 'moved-mono.ttf'
            regular.rename(moved)
            start('--browse', dst)
            key('ctrl+alt+f')
            snap('08-restored-family-style')
            key('Return')
            until(lambda: setting('ui.font.file') == str(moved))
            stop()
            # With no fonts installed, the UI and chooser use embedded monospace.
            for file in fontdir.iterdir():
                file.unlink()
            start('--browse', dst)
            key('ctrl+alt+f')
            snap('09-no-system-fonts-fallback')
            key('Home', 'Return')
            until(lambda: setting('ui.font.file') == '')
            stop()
            print('PASS mouse / keyboard / global monospace / TTF / OTF / non-first TTC face / '
                  'cancel / filter / read error / refresh / persistence / moved and missing fonts', flush=True)
        finally:
            if p and p.poll() is None:
                p.kill()
                p.wait(timeout=5)
            x.terminate()
            x.wait(timeout=5)
