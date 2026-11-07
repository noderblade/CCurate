#!/usr/bin/env python3
"""Exercise app-local CPU rendering on a Windows runner with no installed ICD."""
from pathlib import Path
import ctypes as c
from ctypes import wintypes as w
import os
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
from PIL import ImageGrab

APP, OUTPUT = (Path(p).resolve() for p in sys.argv[1:3])
PROBE = Path(sys.argv[3]).resolve() if len(sys.argv) > 3 else None
sys.argv[1:] = []
OUTPUT.mkdir(parents=True, exist_ok=True)
user32 = c.WinDLL('user32', use_last_error=True)
user32.FindWindowW.argtypes = [w.LPCWSTR, w.LPCWSTR]
user32.FindWindowW.restype = w.HWND
user32.GetWindowThreadProcessId.argtypes = [w.HWND, c.POINTER(w.DWORD)]
user32.IsWindowVisible.argtypes = [w.HWND]
user32.SetForegroundWindow.argtypes = [w.HWND]
user32.GetWindowRect.argtypes = [w.HWND, c.POINTER(w.RECT)]
user32.PostMessageW.argtypes = [w.HWND, w.UINT, w.WPARAM, w.LPARAM]
user32.SetProcessDPIAware()


class SoftwareRendering(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='ccurate-software-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.logs = OUTPUT / self._testMethodName
        self.logs.mkdir(exist_ok=True)
        self.env = dict(os.environ, CCURATE_LOG_DIR=str(self.logs))
        # Invalid overrides do not interfere with direct-driver loading, even
        # for ordinary users. Elevated Windows processes ignore these overrides.
        self.env.update(VK_DRIVER_FILES=str(self.root / 'absent.json'),
                        VK_ICD_FILENAMES=str(self.root / 'absent.json'))

    def text(self):
        return '\n'.join(p.read_text(encoding='utf-8', errors='replace')
                         for p in self.logs.glob('startup-*.log'))

    def window(self, process):
        hwnd = user32.FindWindowW(None, 'CCurate - choose folders')
        pid = w.DWORD()
        if hwnd:
            user32.GetWindowThreadProcessId(hwnd, c.byref(pid))
        return hwnd if pid.value == process.pid and user32.IsWindowVisible(hwnd) else None

    def render(self, mode):
        env = dict(self.env, CCURATE_RENDERER=mode)
        with (self.logs / 'console.log').open('wb') as console:
            process = subprocess.Popen([str(APP)], cwd=self.root, env=env, stdout=console, stderr=console)
            try:
                end = time.monotonic() + 45
                hwnd = None
                while time.monotonic() < end:
                    self.assertIsNone(process.poll(), self.text())
                    hwnd = self.window(process)
                    if hwnd and 'First GUI frame completed' in self.text():
                        break
                    time.sleep(.05)
                else:
                    self.fail('Software renderer did not produce a window/frame\n' + self.text())
                user32.SetForegroundWindow(hwnd)
                time.sleep(.8)
                rect = w.RECT(); self.assertTrue(user32.GetWindowRect(hwnd, c.byref(rect)))
                picture = ImageGrab.grab(bbox=(rect.left, rect.top, rect.right, rect.bottom))
                picture.save(self.logs / 'folder-chooser.png')
                self.assertGreater(len(picture.resize((128, 80)).getcolors(10240) or []), 12,
                                   'Blank software-rendered window')
                user32.PostMessageW(hwnd, 0x0010, 0, 0)  # WM_CLOSE: normal application shutdown.
                self.assertEqual(process.wait(timeout=20), 0, self.text())
            finally:
                if process.poll() is None:
                    process.kill(); process.wait(timeout=10)
        log = self.text()
        self.assertIn('Software rendering active (CPU / Mesa Lavapipe)', log)
        self.assertIn('CPU, packaged Lavapipe', log)
        self.assertIn('first_frame=yes', log)
        self.assertFalse(list(self.root.rglob('.ccurate')))
        return log

    def test_automatic_fallback_without_installed_driver(self):
        log = self.render('auto')
        self.assertIn('System Vulkan initialization failed; trying packaged software rendering', log)

    def test_explicit_software_mode(self):
        log = self.render('software')
        self.assertNotIn('Trying system Vulkan rendering', log)

    def test_missing_cpu_driver_is_reported_and_cli_still_works(self):
        broken = self.root / 'incomplete package Ω'; broken.mkdir()
        shutil.copy2(APP, broken / 'ccurate.exe')
        for dll in APP.parent.glob('*.dll'):
            shutil.copy2(dll, broken / dll.name)
        env = dict(self.env, CCURATE_RENDERER='software')
        result = subprocess.run([str(broken / 'ccurate.exe')], capture_output=True,
                                cwd=self.root, env=env, timeout=30)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('Cannot load packaged software-vulkan', self.text())
        self.assertIn('first_frame=no', self.text())
        result = subprocess.run([str(broken / 'ccurate.exe'), '--version'], capture_output=True,
                                cwd=self.root, env=env, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_clickable_software_launcher_sets_mode_and_preserves_exit(self):
        command = APP.parent / 'CCurate-Software.cmd'
        if not command.exists():
            shutil.copy2(Path(__file__).resolve().parent.parent / 'scripts/windows/CCurate-Software.cmd', command)
            self.addCleanup(command.unlink)
        shell = str(Path(os.environ['SystemRoot']) / 'System32/cmd.exe').replace('/', '\\')
        result = subprocess.run([shell, '/d', '/c', 'call', str(command).replace('/', '\\'), '--version'],
                                capture_output=True, cwd=self.root, env=self.env, timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('CCURATE_RENDERER: software', self.text())
        self.assertIn('Application exited: status=0', self.text())

    @unittest.skipUnless(PROBE, 'Lifecycle probe is tested in the native build')
    def test_repeated_initialization_and_shutdown(self):
        result = subprocess.run([str(PROBE)], capture_output=True, cwd=self.root,
                                env=dict(self.env, CCURATE_RENDERER='software'), timeout=45)
        (self.logs / 'lifecycle.log').write_bytes(result.stdout + result.stderr)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(b'PASS four software-device', result.stdout)


if __name__ == '__main__':
    unittest.main(verbosity=2)
