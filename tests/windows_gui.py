#!/usr/bin/env python3
"""Real Win32/Vulkan smoke test, using only disposable source/destination data."""
from pathlib import Path
from contextlib import closing
import ctypes as c
from ctypes import wintypes as w
import os
import sqlite3
import subprocess
import sys
import tempfile
import time
from PIL import ImageGrab

APP = str(Path(sys.argv[1]).resolve())
SCREENSHOTS = Path(sys.argv[2]).resolve()
SCREENSHOTS.mkdir(parents=True, exist_ok=True)
u = c.WinDLL('user32', use_last_error=True)
u.FindWindowW.argtypes = [w.LPCWSTR, w.LPCWSTR]; u.FindWindowW.restype = w.HWND
u.SetForegroundWindow.argtypes = [w.HWND]
u.MoveWindow.argtypes = [w.HWND, c.c_int, c.c_int, c.c_int, c.c_int, w.BOOL]
u.GetWindowRect.argtypes = [w.HWND, c.POINTER(w.RECT)]
u.PostMessageW.argtypes = [w.HWND, w.UINT, w.WPARAM, w.LPARAM]
u.SetProcessDPIAware()

def key(vk, *modifiers):
    for m in modifiers: u.keybd_event(m, 0, 0, 0)
    u.keybd_event(vk, 0, 0, 0); time.sleep(.03); u.keybd_event(vk, 0, 2, 0)
    for m in reversed(modifiers): u.keybd_event(m, 0, 2, 0)
    time.sleep(.25)

def snap(name, window):
    rect = w.RECT(); assert u.GetWindowRect(window, c.byref(rect))
    image = ImageGrab.grab(bbox=(rect.left, rect.top, rect.right, rect.bottom))
    image.save(SCREENSHOTS / (name + '.png'))
    assert len(image.resize((128, 80)).getcolors(10240) or []) > 12, 'Blank application window'

with tempfile.TemporaryDirectory(prefix='ccurate-windows-gui-') as temporary:
    root = Path(temporary); source, dest = root / 'old', root / 'new'
    source.mkdir(); dest.mkdir()
    original = b'Native Windows curation fixture.\r\n'
    (source / 'document.txt').write_bytes(original)
    logpath = SCREENSHOTS / 'windows-gui.log'
    with logpath.open('wb') as log:
        process = None
        def until(condition, timeout=40):
            end = time.monotonic() + timeout
            while time.monotonic() < end:
                assert process.poll() is None, logpath.read_text(errors='replace')
                try:
                    value = condition()
                    if value: return value
                except (OSError, sqlite3.Error): pass
                time.sleep(.1)
            raise AssertionError('GUI operation timeout\n' + logpath.read_text(errors='replace'))

        def launch(*args, title):
            global process
            process = subprocess.Popen([APP, *map(str, args)], cwd=root, stdout=log, stderr=log)
            hwnd = until(lambda: u.FindWindowW(None, title))
            u.MoveWindow(hwnd, 20, 20, 1200, 850, True)
            u.SetForegroundWindow(hwnd); time.sleep(1)
            return hwnd

        def stop(vk=0x79):
            key(vk); process.wait(timeout=20)
            assert process.returncode == 0, logpath.read_text(errors='replace')

        try:
            win = launch(title='CCurate - choose folders')
            snap('01-folder-chooser', win)
            key(ord('S'), 0x12)
            dialog = until(lambda: u.FindWindowW(None, 'Choose the source folder'))
            snap('02-native-folder-browser', dialog)
            u.SetForegroundWindow(dialog); key(0x1b)
            u.SetForegroundWindow(win); stop(0x1b)
            assert not list(root.rglob('.ccurate'))
            print('PASS native folder dialog / cancel without session writes', flush=True)

            win = launch(source, dest, title='CCurate - manual archive curation')
            activity = dest / '.ccurate/activity.log'
            until(lambda: activity.exists() and 'operation.finish' in activity.read_text(errors='replace'))
            snap('03-workspace', win)
            key(0x74)  # F5: source selection to current destination.
            until(lambda: (dest / 'document.txt').exists())
            assert (dest / 'document.txt').read_bytes() == original
            key(ord('P'), 0x11)
            def reviewed():
                with closing(sqlite3.connect(dest / '.ccurate/catalog.sqlite')) as db:
                    return db.execute("SELECT reviewed FROM entries WHERE path='document.txt'").fetchone() == (1,)
            until(reviewed)
            key(0x70); snap('04-help', win); key(0x70)
            key(ord('H'), 0x11); snap('05-history', win)
            stop()
            assert (source / 'document.txt').read_bytes() == original
            result = subprocess.run([APP, '--cli', str(source), str(dest), 'audit'], capture_output=True, timeout=60)
            assert result.returncode == 0, result.stderr
            print('PASS Win32 Vulkan rendering / F5 / review / history / save / audit', flush=True)
        finally:
            if process and process.poll() is None:
                process.kill(); process.wait(timeout=10)
