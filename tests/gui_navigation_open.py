#!/usr/bin/env python3
"""Native GUI regression for paged back-navigation and all external-open entry points.
Requires Xvfb, xdotool, Pillow and Vulkan. Only generated files/defaults are used.
"""
from pathlib import Path
from PIL import Image, ImageGrab
from desktop_fixture import desktop_fixture
import hashlib
import json
import re
import sqlite3
import subprocess as sp
import sys
import tempfile
import time

app = Path(sys.argv[1]).resolve()
screens = Path(sys.argv[2]).resolve() if len(sys.argv)>2 else None
if screens: screens.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(prefix='ccurate-navigation-') as temp:
    root = Path(temp); src = root/'source'; dst = root/'destination'
    src.mkdir(); dst.mkdir()
    for base in (src,dst):
        (base/'big').mkdir()
        for index in range(2200): (base/'big'/f'd{index:04}').mkdir()
    (src/'files').mkdir()
    Image.new('RGB',(1600,1000),(30,160,80)).save(src/'files/a photo.jpg')
    (src/'files/b note.txt').write_text('Open with the desktop association.\n')
    env, record = desktop_fixture(root)
    env.update(DISPLAY='127.0.0.1:193', RAVEN_WINDOW_SYSTEM='x11')
    def cli(*args): sp.run([app,'--cli',src,dst,*args],env=env,check=True,stdout=sp.DEVNULL)
    cli('scan'); cli('copy','files','Curated','--complete')
    hashes = {p:hashlib.sha512(p.read_bytes()).digest() for p in (src/'files').iterdir()}
    with (root/'x.log').open('w') as xl, (root/'app.log').open('w') as al:
        x = sp.Popen(['Xvfb',':193','-screen','0','1920x1200x24','-nolisten','unix',
                      '-nolisten','local','-listen','tcp','-ac'],env=env,stdout=xl,stderr=xl)
        p = None
        try:
            time.sleep(.5); p = sp.Popen([app,src,dst],env=env,stdout=al,stderr=al)
            def xd(*args):
                return sp.run(['xdotool',*map(str,args)],env=env,check=True,capture_output=True).stdout.decode().strip()
            log = dst/'.ccurate/activity.log'
            def snap(name):
                if screens: ImageGrab.grab(xdisplay=env['DISPLAY']).save(screens/(name+'.png'))
            def until(check):
                end = time.monotonic()+20
                while True:
                    assert p.poll() is None,(root/'app.log').read_text()
                    try:
                        if check(): break
                    except (FileNotFoundError,sp.CalledProcessError,IndexError): pass
                    if time.monotonic()>end:
                        snap('ERROR'); raise AssertionError(log.read_text()[-5000:])
                    time.sleep(.04)
                time.sleep(.2)
            until(lambda: xd('search','--onlyvisible','--name','CCurate'))
            win=xd('search','--onlyvisible','--name','CCurate').splitlines()[-1]
            xd('windowfocus',win); xd('windowsize',win,1920,1200)
            until(lambda: 'operation.finish' in log.read_text()); time.sleep(.6)
            def go(path):
                xd('key','ctrl+g'); time.sleep(.2); xd('key','ctrl+a')
                if path: xd('type',path)
                else: xd('key','BackSpace')
                xd('key','Return'); time.sleep(.6)
            def count(event): return log.read_text().count(' | '+event+' | ')
            def opened(expected):
                until(lambda: record.exists() and len(record.read_text().splitlines()) >= expected)
                until(lambda: count('open.success') >= expected)
                return json.loads(record.read_text().splitlines()[-1])[0]
            # Enter a scrolled directory on the second page, then go back twice.
            for side in (1,0):
                if side==0: xd('key','Tab')
                go('big'); xd('key','ctrl+Next'); time.sleep(.5)
                xd('key','--repeat',72,'--delay',6,'Down'); time.sleep(.3)
                before=count('navigation.change')
                xd('key','Return'); until(lambda: count('navigation.change')>before)
                entry=[line for line in log.read_text().splitlines() if ' | navigation.change | ' in line][-1]
                assert 'target="big/d1071"' in entry,entry
                scroll=int(re.search(r'scroll (\d+)',entry)[1]); assert scroll>0
                for iteration in range(2):
                    before=count('navigation.restore'); xd('key','BackSpace')
                    until(lambda: count('navigation.restore')>before)
                    restored=[line for line in log.read_text().splitlines() if ' | navigation.restore | ' in line][-1]
                    assert f'Pane {side}: page 1, row 72, scroll {scroll}' in restored,restored
                    assert 'path="big/d1071"' in restored,restored
                    snap(f'back-pane-{side}-{iteration}')
                    before=count('navigation.change'); xd('key','Return')
                    until(lambda: count('navigation.change')>before)
                    assert 'target="big/d1071"' in [line for line in log.read_text().splitlines() if ' | navigation.change | ' in line][-1]
                # Direct jump without a parent bookmark must still find the child.
                go('big/d2090'); before=count('navigation.restore'); xd('key','BackSpace')
                until(lambda: count('navigation.restore')>before)
                restored=[line for line in log.read_text().splitlines() if ' | navigation.restore | ' in line][-1]
                assert 'path="big/d2090"' in restored and 'page 2, row 91' in restored,restored
            print('PASS both panes: second-page focus + exact scroll, repeated back/enter, direct jump to third-page child',flush=True)
            xd('key','Tab'); go('files')
            # A hover must neither launch nor change the selected file.
            xd('key','Home'); xd('key','Down'); xd('mousemove',1100,280); time.sleep(1)
            assert not record.exists()
            xd('key','ctrl+Return'); assert opened(1)==str(src/'files/a photo.jpg')
            # Double-click the second file (parent row + image row precede it).
            xd('mousemove',1100,280); xd('click','--repeat',2,'--delay',80,1)
            assert opened(2)==str(src/'files/b note.txt')
            time.sleep(.5); assert len(record.read_text().splitlines())==2
            xd('key','F9'); time.sleep(.2); xd('type','open'); xd('key','Return')
            assert opened(3)==str(src/'files/b note.txt')
            # Files menu first item is the same operation.
            xd('key','F2'); xd('key','Home'); xd('key','Return'); xd('key','Home'); xd('key','Return')
            assert opened(4)==str(src/'files/b note.txt')
            # History resolves the original path to the retained physical destination.
            xd('key','ctrl+h'); time.sleep(.7); xd('key','ctrl+Return')
            with sqlite3.connect(dst/'.ccurate/catalog.sqlite') as db:
                relative=db.execute("select o.dest from entries e join versions v on v.eid=e.id join objects o on o.hash=v.hash where e.path='files/b note.txt' and v.accepted=1").fetchone()[0]
            assert opened(5)==str(dst/relative)
            xd('key','Tab'); go(str(Path(relative).parent)); xd('key','End'); time.sleep(.2)
            xd('key','ctrl+Return'); assert opened(6)==str(dst/relative)
            snap('opened-in-default-application')
            (dst/relative).unlink()
            xd('key','ctrl+Return'); until(lambda: count('open.failed')==1)
            assert 'Cannot open this regular file' in log.read_text()
            assert len(record.read_text().splitlines())==6
            for path,before in hashes.items(): assert hashlib.sha512(path.read_bytes()).digest()==before
            xd('key','F10'); p.wait(timeout=20); assert p.returncode==0
            print('PASS hover / double-click / Ctrl+Enter / F9 open / Files menu / source / destination / history / unchanged source',flush=True)
        finally:
            if p and p.poll() is None: p.kill(); p.wait(timeout=5)
            x.terminate(); x.wait(timeout=5)
