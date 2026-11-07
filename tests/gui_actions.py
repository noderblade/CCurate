#!/usr/bin/env python3
"""Optional real-window mouse / splitter / live-first-scan regression.
Usage: python3 tests/gui_actions.py build/ccurate [screenshots-dir]
Requires Xvfb, xdotool, Pillow, and a Vulkan driver. Uses only disposable files.
"""
from pathlib import Path
import os, subprocess as sp, sqlite3, sys, tempfile, time
from PIL import ImageGrab
app=Path(sys.argv[1]).resolve()
screens=Path(sys.argv[2]).resolve() if len(sys.argv)>2 else None
if screens: screens.mkdir(parents=True,exist_ok=True)
with tempfile.TemporaryDirectory(prefix='ccurate-actions-') as t:
 root=Path(t); src=root/'source'; dst=root/'destination'; src.mkdir(); dst.mkdir()
 for i in range(20000): (src/f'doc-{i:05}.txt').write_text(f'Archive document {i}\n')
 env=os.environ.copy(); env.update(DISPLAY='127.0.0.1:190',RAVEN_WINDOW_SYSTEM='x11')
 gate,ready,hook=root/'enumeration.gate',root/'enumeration.ready',root/'enumeration_gate.so'
 gate.touch()
 sp.run(['cc','-shared','-fPIC','-O2','-Wall','-Wextra',
         str(Path(__file__).with_name('enumeration_gate.c')),'-ldl','-o',str(hook)],check=True)
 app_env=dict(env,CCURATE_TEST_ENUM_GATE=str(gate),CCURATE_TEST_ENUM_READY=str(ready),
              LD_PRELOAD=':'.join(filter(None,[env.get('LD_PRELOAD'),str(hook)])))
 with (root/'x.log').open('w') as xl, (root/'app.log').open('w') as al:
  x=sp.Popen(['Xvfb',':190','-screen','0','1920x1200x24','-nolisten','unix','-nolisten','local','-listen','tcp','-ac'],env=env,stdout=xl,stderr=xl)
  p=None
  try:
   time.sleep(.5); p=sp.Popen([app,src,dst],env=app_env,stdout=al,stderr=al)
   def xd(*args): return sp.run(['xdotool',*map(str,args)],env=env,check=True,capture_output=True).stdout.decode().strip()
   def snap(name):
    if screens: ImageGrab.grab(xdisplay=env['DISPLAY']).save(screens/(name+'.png'))
    print(name,flush=True)
   def until(check,timeout=30):
    end=time.monotonic()+timeout
    while True:
     assert p.poll() is None, (root/'app.log').read_text()
     try:
      if check(): break
     except (sqlite3.OperationalError,IndexError,sp.CalledProcessError): pass
     if time.monotonic()>end: snap('ERROR-timeout'); raise AssertionError('Operation timeout')
     time.sleep(.03)
    time.sleep(.12)
   def sql(q):
    with sqlite3.connect(dst/'.ccurate/catalog.sqlite',timeout=.1) as db: return db.execute(q).fetchone()
   until(lambda: xd('search','--name','CCurate'))
   win=xd('search','--name','CCurate').splitlines()[-1]
   xd('windowfocus',win); xd('windowsize',win,1920,1200)
   until(lambda: ready.exists())
   until(lambda: sql('select count(*) from discoveries')[0]>128)
   time.sleep(1.1)
   assert sql('select count(*) from versions')==(0,), 'Expected enumeration to still be running'
   snap('01-live-first-scan')
   def click(x,y): xd('mousemove',x,y); xd('click',1); time.sleep(.18)
   counts=[8,6,18,8,8,8,4]
   def menu(group,row=None,hover=False):
    click(12+(group+1.5)*237,1180)
    if row is not None:
     n=counts[group]; h=42+n*29+100; y=1200-42-h+40+row*29+14
     px=min(12+(group+1)*237,1920-660-12)+30
     xd('mousemove',int(px),int(y)); time.sleep(.25)
     if not hover: xd('click',1); time.sleep(.2)
   menu(1,0,True); snap('02-scan-menu-help')
   # Cancel through the mouse menu (last row), not through Escape.
   click(520,1200-42-(42+6*29+100)+40+5*29+14)
   gate.unlink()  # Let the scanner observe the cancellation requested by the UI.
   until(lambda: 'Scan cancelled' in (dst/'.ccurate/activity.log').read_text())
   time.sleep(.7)
   assert sql("select value from settings where key='scan_complete'")==('0',)
   menu(1,0)
   until(lambda: sql("select value from settings where key='scan_complete'")==('1',),60)
   time.sleep(.8)
   for group in range(7):
    menu(group,0,True); xd('key','Escape'); time.sleep(.1)
   # File copy using only mouse commands after choosing the source row.
   click(1080,230); menu(0,1)
   until(lambda: (dst/'doc-00000.txt').exists())
   assert (dst/'doc-00000.txt').read_bytes()==(src/'doc-00000.txt').read_bytes()
   menu(3,4)
   until(lambda: sql("select reviewed from entries where path='doc-00000.txt'")== (1,))
   index=dst/'.ccurate/archive.jsonl'; before=index.stat().st_mtime_ns
   menu(1,4); until(lambda: index.stat().st_mtime_ns!=before)
   menu(6,1); time.sleep(.5); snap('03-balance-menu')
   def drag(x0,y0,x1,y1):
    xd('mousemove',x0,y0); xd('mousedown',1); time.sleep(.08)
    xd('mousemove',x1,y1); time.sleep(.12); xd('mouseup',1); time.sleep(.3)
   drag(960,350,1200,350); drag(960,900,650,900); drag(800,683,800,800)
   def split(key): return float(sql(f"select value from settings where key='ui.split.{key}'")[0])
   until(lambda: .62<split('top')<.64 and .33<split('bottom')<.35 and .64<split('rows')<.66)
   snap('04-three-independent-splits')
   xd('mousemove',1200,350); xd('click','--repeat',2,'--delay',70,1)
   until(lambda: split('top')==.5)
   assert .33<split('bottom')<.35
   # Keyboard reaches every menu category and configures the same layout.
   xd('key','F2'); xd('key','End'); xd('key','Return'); xd('key','Home'); xd('key','Return'); time.sleep(.2)
   xd('key','ctrl+alt+l'); time.sleep(.2); xd('key','ctrl+a'); xd('type','40 60 55 25'); xd('key','Return')
   until(lambda: split('top')==.4 and split('bottom')==.6 and split('rows')==.55)
   menu(0,4); time.sleep(.3); xd('type','Mouse-created'); snap('05-mouse-confirm-dialog')
   click(790,635)
   until(lambda: (dst/'Mouse-created').is_dir())
   # Double-click must open the left directory, not merely select its row.
   xd('mousemove',120,254); xd('click','--repeat',2,'--delay',70,1); time.sleep(.4)
   menu(0,4); xd('type','Nested'); click(790,635)
   until(lambda: (dst/'Mouse-created/Nested').is_dir())
   menu(2,6); time.sleep(.3)
   menu(4,2); time.sleep(.3)
   menu(5,0); time.sleep(.2); xd('key','ctrl+a'); xd('type','200'); xd('key','Return'); time.sleep(.6)
   xd('key','F2'); xd('key','End'); xd('key','Return'); time.sleep(.3); snap('06-menu-at-200-percent'); xd('key','Escape')
   xd('key','ctrl+0'); time.sleep(.3)
   # Save through Files menu; restart in history to inspect persisted proportions.
   menu(0,7); p.wait(timeout=20); assert p.returncode==0
   assert split('top')==.4 and split('bottom')==.6 and split('rows')==.55
   p=sp.Popen([app,'--browse',dst],env=env,stdout=al,stderr=al)
   until(lambda: xd('search','--name','CCurate'))
   win=xd('search','--name','CCurate').splitlines()[-1]
   xd('windowfocus',win); xd('windowsize',win,1920,1200); time.sleep(.8); snap('07-restored-layout')
   xd('key','F10'); p.wait(timeout=15); assert p.returncode==0
   print('live window / all menu categories / hover help / mouse commands / three splitters / keyboard layout / persistence PASS',flush=True)
  finally:
   if p and p.poll() is None: p.kill(); p.wait(timeout=5)
   x.terminate(); x.wait(timeout=5)
