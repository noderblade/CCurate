#!/usr/bin/env python3
"""Optional native GUI smoke test: python3 tests/gui_smoke.py build/ccurate [screenshots-dir].
Requires Xvfb, xdotool, Pillow and a working Vulkan driver (software is sufficient).
Uses disposable directories, hashes part of a 16 GiB sparse fixture, then cancels.
"""
from pathlib import Path
import os, subprocess as sp, time, sqlite3, sys, tempfile
from PIL import ImageGrab
temp = tempfile.TemporaryDirectory(prefix='ccurate-gui-')
root=Path(temp.name)
screens=Path(sys.argv[2]).resolve() if len(sys.argv)>2 else None
if screens: screens.mkdir(parents=True,exist_ok=True)
src=root/'source'; dst=root/'destination'; src.mkdir(); dst.mkdir()
app=Path(sys.argv[1]).resolve()
env=os.environ.copy(); env.update(DISPLAY='127.0.0.1:189',RAVEN_WINDOW_SYSTEM='x11')
for n in range(512): (src/f'doc-{n:04}.txt').write_text(f'Important test document {n}\n')
sp.run([app,'--cli',src,dst,'scan'],env=env,check=True,stdout=sp.DEVNULL)
sp.run([app,'--cli',src,dst,'copy','doc-0000.txt',''],env=env,check=True,stdout=sp.DEVNULL)
with (src/'zz-large.bin').open('wb') as f: f.truncate(16*1024**3)
xlog=(root/'xvfb.log').open('w'); alog=(root/'app.log').open('w')
x=sp.Popen(['Xvfb',':189','-screen','0','3840x2160x24','-nolisten','unix','-nolisten','local','-listen','tcp','-ac'],env=env,stdout=xlog,stderr=xlog)
p=None
try:
 time.sleep(.5)
 p=sp.Popen([app,src,dst],env=env,stdout=alog,stderr=alog)
 def xd(*args): return sp.run(['xdotool',*map(str,args)],env=env,check=True,capture_output=True).stdout.decode().strip()
 deadline=time.monotonic()+20
 while True:
  try:
   win=xd('search','--onlyvisible','--name','CCurate').splitlines()[-1]; break
  except (sp.CalledProcessError,IndexError):
   assert p.poll() is None, (root/'app.log').read_text()
   assert time.monotonic()<deadline, 'Window timeout'
   time.sleep(.2)
 xd('windowfocus',win); xd('windowsize',win,1920,1200)
 time.sleep(1)
 def snap(name):

  if screens: ImageGrab.grab(xdisplay=env['DISPLAY']).save(screens/(name+'.png'))
  print(name,flush=True)
 snap('01-progress-100')
 xd('key','ctrl+comma'); time.sleep(.3); xd('key','ctrl+a'); xd('type','200'); xd('key','Return'); time.sleep(.6)
 snap('02-progress-200')
 xd('key','ctrl+comma'); time.sleep(.3); xd('key','ctrl+a'); xd('type','400'); xd('key','Return'); xd('windowsize',win,3840,2160); time.sleep(1)
 snap('03-progress-400')
 xd('key','ctrl+0'); xd('windowsize',win,1920,1200); time.sleep(.3)
 xd('key','F1'); time.sleep(.4); snap('04-help-during-scan')
 xd('key','F1'); xd('key','Tab'); xd('key','Tab'); xd('key','Down'); time.sleep(.3)
 xd('key','Escape'); time.sleep(1)
 snap('05-stopped')
 def until(condition):
  end=time.monotonic()+10
  while not condition():
   assert p.poll() is None

   if time.monotonic()>=end:
    snap('ERROR-timeout'); raise AssertionError('Operation timeout')
   time.sleep(.05)
  time.sleep(.2)
 def sql(statement):
  with sqlite3.connect(dst/'.ccurate/catalog.sqlite') as db:
   return db.execute(statement).fetchone()
 xd('key','Escape'); xd('key','F5')
 until(lambda: (dst/'doc-0001.txt').exists())
 assert (dst/'doc-0001.txt').read_bytes()==(src/'doc-0001.txt').read_bytes()
 xd('key','ctrl+p'); until(lambda: sql("select reviewed from entries where path='doc-0001.txt'")== (1,))
 xd('key','F8'); until(lambda: sql("select discarded from entries where path='doc-0001.txt'")== (1,))
 assert (src/'doc-0001.txt').exists()
 xd('key','shift+F8'); until(lambda: sql("select discarded from entries where path='doc-0001.txt'")== (0,))
 xd('key','F7'); time.sleep(.2); xd('type','Curated'); xd('key','Return')
 until(lambda: (dst/'Curated').is_dir())
 xd('key','Tab'); xd('key','slash'); time.sleep(.2); xd('type','doc-0001'); xd('key','Return'); time.sleep(.5)
 xd('key','F6'); time.sleep(.2); xd('key','ctrl+a'); xd('type','Curated/doc-0001.txt'); xd('key','Return')
 until(lambda: (dst/'Curated/doc-0001.txt').is_file())
 assert sql("select dest from objects where dest='Curated/doc-0001.txt'")==('Curated/doc-0001.txt',)
 assert not (dst/'doc-0001.txt').exists()
 xd('key','ctrl+h'); time.sleep(.5); snap('06-curated-history')
 print('GUI F5 / review / reject / restore / F7 / F6 / history PASS',flush=True)
 xd('key','ctrl+comma'); time.sleep(.3); xd('key','ctrl+a'); xd('type','250'); xd('key','Return'); time.sleep(.5)
 xd('key','F10'); p.wait(timeout=15)
 with sqlite3.connect(dst/'.ccurate/catalog.sqlite') as db:
  scale=db.execute("select value from settings where key='ui.scale'").fetchone()
  print('persisted scale',scale,flush=True)
  assert scale==('2.500',)
 assert p.returncode==0
 print('GUI keyboard / scale / cancel / save PASS',flush=True)
finally:
 if p and p.poll() is None: p.kill(); p.wait(timeout=5)
 x.terminate(); x.wait(timeout=5)
 (src/'zz-large.bin').unlink(missing_ok=True)
 temp.cleanup()
