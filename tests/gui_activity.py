#!/usr/bin/env python3
"""Real GUI: click-only selection, oriented JPEG, logs, themes, fourth splitter.
Requires Xvfb, xdotool, Pillow and Vulkan. Every file is a disposable fixture.
"""
from pathlib import Path
from PIL import Image, ImageGrab
import os, sqlite3, subprocess as sp, sys, tempfile, time
app=Path(sys.argv[1]).resolve()
screens=Path(sys.argv[2]).resolve() if len(sys.argv)>2 else None
if screens: screens.mkdir(parents=True,exist_ok=True)
with tempfile.TemporaryDirectory(prefix='ccurate-new-ui-') as t:
 root=Path(t); src=root/'source'; dst=root/'destination'; src.mkdir(); dst.mkdir()
 exif=Image.Exif(); exif[274]=6
 Image.new('RGB',(1280,800),(20,190,95)).save(src/'a-photo.jpg',exif=exif)
 (src/'b-text.txt').write_text('Click, not hover, selects this file.\n')
 (src/'c-broken.jpg').write_bytes((src/'a-photo.jpg').read_bytes()[:100])
 (src/'d-denied.txt').write_text('Unreadable fixture'); (src/'d-denied.txt').chmod(0)
 env=os.environ.copy(); env.update(DISPLAY='127.0.0.1:191',RAVEN_WINDOW_SYSTEM='x11')
 with (root/'x.log').open('w') as xl, (root/'app.log').open('w') as al:
  x=sp.Popen(['Xvfb',':191','-screen','0','1920x1200x24','-nolisten','unix','-nolisten','local','-listen','tcp','-ac'],env=env,stdout=xl,stderr=xl)
  p=None
  try:
   time.sleep(.5); p=sp.Popen([app,src,dst],env=env,stdout=al,stderr=al)
   def xd(*args): return sp.run(['xdotool',*map(str,args)],env=env,check=True,capture_output=True).stdout.decode().strip()
   def snap(name):
    image=ImageGrab.grab(xdisplay=env['DISPLAY'])
    if screens: image.save(screens/(name+'.png'))
    print(name,flush=True); return image
   def until(check,timeout=20):
    end=time.monotonic()+timeout
    while True:
     assert p.poll() is None,(root/'app.log').read_text()
     try:
      if check(): break
     except (sqlite3.OperationalError,IndexError,FileNotFoundError,sp.CalledProcessError): pass
     if time.monotonic()>end: snap('ERROR-timeout'); raise AssertionError('Operation timeout')
     time.sleep(.05)
    time.sleep(.15)
   def sql(q):
    with sqlite3.connect(dst/'.ccurate/catalog.sqlite',timeout=.2) as db: return db.execute(q).fetchone()
   def setting(key): return sql("select value from settings where key='"+key+"'")[0]
   log=dst/'.ccurate/activity.log'
   until(lambda: xd('search','--name','CCurate'))
   win=xd('search','--name','CCurate').splitlines()[-1]
   xd('windowfocus',win); xd('windowsize',win,1920,1200)
   until(lambda: 'operation.finish' in log.read_text()); time.sleep(.6)
   def click(x,y): xd('mousemove',x,y); xd('click',1); time.sleep(.2)
   counts=[8,6,16,8,8,8,4]
   def menu(group,row):
    click(12+(group+1.5)*237,1180)
    h=42+counts[group]*29+100
    y=1200-42-h+40+row*29+14
    px=min(12+(group+1)*237,1920-660-12)+30
    click(px,y)
   click(1080,230)
   until(lambda: 'Preview 500 x 800' in log.read_text())
   im=snap('01-oriented-jpeg')
   crop=im.crop((20,710,940,1140))
   pixels=getattr(crop,'get_flattened_data',crop.getdata)()
   green=sum(1 for r,g,b in pixels if g>140 and r<60 and b<130)
   assert green>20000,green
   xd('mousemove',1080,255); time.sleep(1.3); xd('key','F5')
   until(lambda: (dst/'a-photo.jpg').exists())
   assert not (dst/'b-text.txt').exists(),'Hover changed source selection'
   # A corrupt image produces a specific warning, while scan permission errors are ERROR.
   click(1080,280); until(lambda: 'preview.failed' in log.read_text())
   assert 'Image decoder:' in log.read_text()
   assert ' | ERROR | ' in log.read_text() and 'd-denied.txt' in log.read_text()
   menu(2,12); until(lambda: setting('ui.log.visible')=='1')
   snap('02-activity-all')
   # Log dropdown is selected with the mouse (Errors, fourth option).
   click(1670,925); time.sleep(.2)
   snap('03-log-dropdown')
   click(1530,1090); until(lambda: setting('ui.log.level')=='3')
   snap('04-errors-only')
   xd('key','ctrl+shift+l'); xd('key','Home'); xd('key','Return')
   until(lambda: setting('ui.log.level')=='-1')
   # Fourth splitter: grow the log upward, then reset by double click.
   xd('mousemove',960,902); xd('mousedown',1); xd('mousemove',960,790); xd('mouseup',1)
   until(lambda: .35<float(setting('ui.split.log'))<.38)
   snap('05-resized-log')
   xd('mousemove',960,790); xd('click','--repeat',2,'--delay',70,1)
   until(lambda: float(setting('ui.split.log'))==.25)
   # Theme popup from the mouse menu; select Tango Light by mouse.
   menu(5,6); snap('06-theme-picker'); click(850,543)
   until(lambda: setting('ui.theme')=='2'); snap('07-tango-light')
   xd('key','ctrl+comma'); time.sleep(.2); snap('07-tango-light-dialog'); xd('key','Escape')
   # Remaining themes and keyboard dropdown use.
   for index,name in enumerate(['ccurate-dark','tango-dark','tango-light','solarized-dark','solarized-light','nord','dracula','gruvbox-dark','catppuccin-mocha','tokyo-night']):
    xd('key','ctrl+alt+t'); xd('key','Home')
    for _ in range(index): xd('key','Down')
    xd('key','Return'); until(lambda: setting('ui.theme')==str(index)); snap('08-'+name)
   menu(6,3); snap('09-status-legend')
   xd('key','ctrl+comma'); time.sleep(.2); xd('key','ctrl+a'); xd('type','400'); xd('key','Return'); time.sleep(.3)
   xd('key','ctrl+alt+t'); xd('key','End'); snap('09-theme-picker-at-400'); xd('key','Return'); xd('key','ctrl+0'); time.sleep(.3)
   xd('key','ctrl+alt+l'); time.sleep(.2); xd('key','ctrl+a'); xd('type','45 55 60 30'); xd('key','Return')
   until(lambda: setting('ui.split.log')=='0.3000')
   xd('key','ctrl+shift+d'); xd('key','Home'); xd('key','Right'); xd('key','Page_Down'); xd('key','End')
   xd('key','F10'); p.wait(timeout=20); assert p.returncode==0
   before=log.read_bytes()
   p=sp.Popen([app,'--browse',dst],env=env,stdout=al,stderr=al)
   until(lambda: xd('search','--name','CCurate'))
   win=xd('search','--name','CCurate').splitlines()[-1]; xd('windowfocus',win); xd('windowsize',win,1920,1200); time.sleep(.7)
   assert setting('ui.theme')=='9' and setting('ui.log.visible')=='1' and setting('ui.split.log')=='0.3000'
   assert log.read_bytes().startswith(before)
   snap('10-restored-theme-log-layout'); xd('key','F10'); p.wait(timeout=20); assert p.returncode==0
   print('PASS click-only / JPEG process / log dropdown / four splitters / ten themes / legend / persistence',flush=True)
  finally:
   (src/'d-denied.txt').chmod(0o600)
   if p and p.poll() is None: p.kill(); p.wait(timeout=5)
   x.terminate(); x.wait(timeout=5)
