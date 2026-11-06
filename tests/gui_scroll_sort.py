#!/usr/bin/env python3
"""Real-window scrollbar, header sorting and saved sort preferences regression.
Requires Xvfb, xdotool, Pillow and Vulkan. All files and desktop associations are disposable.
"""
from pathlib import Path
from PIL import ImageGrab
from desktop_fixture import desktop_fixture
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
with tempfile.TemporaryDirectory(prefix='ccurate-scroll-sort-') as tmp:
    root=Path(tmp); src=root/'source'; dst=root/'destination'; src.mkdir(); dst.mkdir()
    for base in (src,dst):
        (base/'many').mkdir(); (base/'wide').mkdir()
        for i in range(2205): (base/'many'/f'f{i:04}.txt').write_bytes(str(i).encode().ljust(32+i%79,b'x'))
    longtext=''.join(f'Line {i:04}: '+('readable text '*20)+'\n' for i in range(120))
    for i in range(60): (src/'wide'/(f'{i:02}-'+'long-name-'*19+'.txt')).write_text(longtext)
    env, record = desktop_fixture(root)
    env.update(DISPLAY='127.0.0.1:194',RAVEN_WINDOW_SYSTEM='x11')
    sp.run([app,'--cli',src,dst,'scan'],env=env,check=True,capture_output=True)
    log=dst/'.ccurate/activity.log'; database=dst/'.ccurate/catalog.sqlite'
    with (root/'x.log').open('w') as xl, (root/'app.log').open('w') as al:
        x=sp.Popen(['Xvfb',':194','-screen','0','1920x1200x24','-nolisten','unix','-nolisten','local','-listen','tcp','-ac'],env=env,stdout=xl,stderr=xl)
        p=None
        try:
            time.sleep(.5); p=sp.Popen([app,src,dst],env=env,stdout=al,stderr=al)
            def xd(*args):
                return sp.run(['xdotool',*map(str,args)],env=env,check=True,capture_output=True).stdout.decode().strip()
            def snap(name):
                if screens: ImageGrab.grab(xdisplay=env['DISPLAY']).save(screens/(name+'.png'))
            def until(check):
                end=time.monotonic()+30
                while True:
                    assert p.poll() is None,(root/'app.log').read_text()
                    try:
                        if check(): break
                    except (FileNotFoundError,sp.CalledProcessError,sqlite3.OperationalError): pass
                    if time.monotonic()>end:
                        snap('ERROR'); raise AssertionError(log.read_text()[-5000:])
                    time.sleep(.03)
                time.sleep(.2)
            def setting(key):
                with sqlite3.connect(database) as db:
                    row=db.execute('SELECT value FROM settings WHERE key=?',(key,)).fetchone()
                    return row[0] if row else None
            def click(x,y): xd('mousemove',round(x),round(y)); xd('click',1); time.sleep(.2)
            def go(path):
                xd('key','ctrl+g'); time.sleep(.15); xd('key','ctrl+a'); xd('type',path); xd('key','Return'); time.sleep(.7)
            until(lambda: xd('search','--onlyvisible','--name','CCurate'))
            win=xd('search','--onlyvisible','--name','CCurate').splitlines()[-1]
            xd('windowfocus',win); xd('windowsize',win,1920,1200)
            until(lambda: 'operation.finish' in log.read_text()); time.sleep(.7)
            go('many'); click(1010,254)
            # Headers share the same hit geometry as the horizontally scrolling table.
            for column,xpos in ((0,1020),(1,1380),(2,1560),(3,1690),(5,1860)):
                click(xpos,199)
                expected=(256 if column==0 else column)
                until(lambda: setting('ui.sort.right')==str(expected))
                click(xpos,199)
                until(lambda: setting('ui.sort.right')==str(column if column==0 else column|256))
            assert 'path="many/f0000.txt"' in log.read_text()
            # Every column is also available without a mouse.
            xd('key','ctrl+s'); time.sleep(.2); xd('key','Home'); xd('key','--repeat',4,'Down'); xd('key','Return')
            until(lambda: setting('ui.sort.right')==str(4|256) and setting('ui.date.right')=='1')
            xd('key','ctrl+shift+s'); until(lambda: setting('ui.sort.right')=='4')
            xd('key','ctrl+t'); until(lambda: setting('ui.sort.right')=='3' and setting('ui.date.right')=='0')
            xd('key','ctrl+F3'); until(lambda: setting('ui.sort.right')=='0')
            def drag(x0,y0,x1,y1):
                xd('mousemove',round(x0),round(y0)); xd('mousedown',1)
                xd('mousemove',round(x1),round(y1)); time.sleep(.3); xd('mouseup',1); time.sleep(.5)
            def scroll_event(ident):
                rows=[r for r in log.read_text().splitlines() if f'Scrollbar {ident}:' in r]
                assert rows,ident
                match=re.search(r'position (\d+) / (\d+)',rows[-1]); return tuple(map(int,match.groups()))
            for side in (1,0):
                if side==0: xd('key','Tab'); go('many')
                bx=1896 if side else 942
                drag(bx,222,bx,632)
                position,maximum=scroll_event(side*2)
                assert maximum>2000 and position>=maximum-1,(side,position,maximum)
                count=len(record.read_text().splitlines()) if record.exists() else 0
                xd('key','ctrl+Return')
                until(lambda: record.exists() and len(record.read_text().splitlines())>count)
                assert '/many/f21' in json.loads(record.read_text().splitlines()[-1])[0]
                snap(f'pane-{side}-global-scroll')
                drag(bx,632,bx,222)
                position,_=scroll_event(side*2); assert position==0
            xd('key','Tab'); go('wide'); click(1010,254); time.sleep(.8)
            # Drag the source horizontal bar to reach all columns behind long filenames.
            drag(982,641,1878,641)
            position,maximum=scroll_event(3); assert position>0 and maximum>500
            snap('wide-table-and-text')
            # Both textual panes expose independent horizontal and vertical bars.
            for ident,x0,y0,x1,y1 in ((4,939,733,939,1137),(5,28,1147,925,1147),
                                       (6,1895,733,1895,1137),(7,982,1147,1879,1147)):
                drag(x0,y0,x1,y1)
                position,maximum=scroll_event(ident); assert position>0 and maximum>0,(ident,position,maximum)
            snap('preview-details-scrolled')
            xd('key','ctrl+l'); time.sleep(.5); xd('key','ctrl+shift+d'); xd('key','Home'); time.sleep(.2)
            drag(1896,975,1896,1134)
            position,maximum=scroll_event(8); assert position>0 and maximum>0
            drag(28,1145,1875,1145)
            position,maximum=scroll_event(9); assert position>0 and maximum>0
            snap('activity-scrolled')
            # Independent pane sort and date preferences survive a new process.
            xd('key','Tab'); xd('key','ctrl+F6'); until(lambda: setting('ui.sort.left')==str(2|256))
            xd('key','Tab'); xd('key','ctrl+s'); time.sleep(.2); xd('key','Home'); xd('key','--repeat',4,'Down'); xd('key','Return')
            until(lambda: setting('ui.date.right')=='1')
            right_sort=setting('ui.sort.right')
            xd('key','F10'); p.wait(timeout=20); assert p.returncode==0
            p=sp.Popen([app,src,dst],env=env,stdout=al,stderr=al)
            until(lambda: xd('search','--onlyvisible','--name','CCurate'))
            win=xd('search','--onlyvisible','--name','CCurate').splitlines()[-1]; xd('windowfocus',win)
            time.sleep(1); xd('key','F10'); p.wait(timeout=20); assert p.returncode==0
            assert setting('ui.sort.left')==str(2|256) and setting('ui.sort.right')==right_sort and setting('ui.date.right')=='1'
            print('PASS header sorting / keyboard sorting / saved pane preferences / global list scrolling / horizontal tables / preview / details / activity')
        finally:
            if p and p.poll() is None: p.kill(); p.wait(timeout=5)
            x.terminate(); x.wait(timeout=5)
