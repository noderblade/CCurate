#!/usr/bin/env python3
"""Windows bootstrap failures, stderr capture and pre-entry loader diagnostics."""
from pathlib import Path
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

APP = Path(sys.argv.pop(1)).resolve()
GUI = sys.argv.pop(1) == '1'
PROBE = Path(sys.argv.pop(1)).resolve() if len(sys.argv) > 1 else None
SCRIPTS = Path(__file__).resolve().parent.parent / 'scripts/windows'


class Startup(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='ccurate-startup-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.logs = self.root / 'logs Ω'
        self.env = dict(os.environ, CCURATE_LOG_DIR=str(self.logs))

    def run_app(self, *args, env=None):
        return subprocess.run([str(APP), *args], capture_output=True, timeout=30,
                              env=env or self.env, cwd=self.root)

    def read_log(self):
        files = list(self.logs.glob('startup-*.log'))
        self.assertEqual(len(files), 1)
        return files[0].read_text(encoding='utf-8')

    def test_plain_start_keeps_redirected_output_and_logs_exit(self):
        result = self.run_app('--version')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(result.stdout.startswith(b'CCurate 0.1.0 / Raven'))
        self.assertNotIn(b'[startup]', result.stdout)
        self.assertIn(b'Startup log:', result.stderr)
        log = self.read_log()
        self.assertIn('Windows version:', log)
        self.assertIn('build ', log)
        self.assertIn('Application exited: status=0', log)
        self.assertIn('Runtime resources configured', log)
        self.assertEqual(list(self.root.glob('.ccurate')), [])

    def test_debug_flag_preserves_application_arguments(self):
        result = self.run_app('--debug', '--version')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(result.stdout.startswith(b'CCurate 0.1.0 / Raven'))
        self.assertIn('debug=yes', self.read_log())
        self.assertIn('VK_LOADER_DEBUG:', self.read_log())

    def test_cli_failure_stderr_is_also_in_file(self):
        result = self.run_app('--cli', str(self.root / 'missing-source'), str(self.root / 'missing-target'), 'scan')
        self.assertNotEqual(result.returncode, 0)
        log = self.read_log()
        self.assertIn('Application exited: status=1', log)
        self.assertTrue('Destination:' in log or 'Windows paths' in log or 'directory' in log.lower(), log)

    def test_workers_do_not_create_logs_or_corrupt_protocol(self):
        for prefix in ([], ['--debug']):
            result = self.run_app(*prefix, '--preview-worker', '0', '0', '1')
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(struct.unpack('<I', result.stdout[:4]), (0x52435032,))
            self.assertNotIn(b'[startup]', result.stderr)
        self.assertFalse(self.logs.exists())

    @unittest.skipUnless(GUI, 'GUI build only')
    def test_missing_vulkan_driver_reports_before_first_window(self):
        bad = str(self.root / 'no-vulkan-driver.json')
        env = dict(self.env, VK_DRIVER_FILES=bad, VK_ICD_FILENAMES=bad,
                   VK_LOADER_DEBUG='error,warn,driver')
        result = self.run_app(env=env)
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn(b'Vulkan', result.stderr)
        log = self.read_log()
        self.assertIn('Initializing the folder chooser', log)
        self.assertIn('Raven', log)
        self.assertIn('first_frame=no', log)
        self.assertNotIn('First GUI frame completed', log)
        self.assertFalse(list(self.root.rglob('.ccurate')))

    @unittest.skipUnless(PROBE, 'Native diagnostics probe not supplied')
    def test_launch_without_any_standard_handles(self):
        result = subprocess.run([str(PROBE), '--launch-detached', str(APP)], capture_output=True,
                                env=self.env, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('Application exited: status=0', self.read_log())

    @unittest.skipUnless(PROBE, 'Native diagnostics probe not supplied')
    def test_unhandled_exception_is_recorded(self):
        result = subprocess.run([str(PROBE)], capture_output=True, env=self.env, timeout=30)
        self.assertEqual(result.returncode & 0xffffffff, 0xC0000005)
        self.assertIn('Unhandled Windows exception 0xC0000005', self.read_log())

    def test_launcher_waits_and_preserves_successful_exit(self):
        for name in ('CCurate-Debug.cmd', 'CCurate-Debug.ps1'):
            script = APP.parent / name
            if not script.exists():
                shutil.copy2(SCRIPTS / name, script)
                self.addCleanup(script.unlink)
        # Exercise the actual double-click launcher, including its PowerShell
        # lookup and propagation of the application exit status.
        command = APP.parent / 'CCurate-Debug.cmd'
        comspec = Path(os.environ['SystemRoot']) / 'System32/cmd.exe'
        cmdline = f'"{comspec}" /d /s /c ""{command}" -NoPause --version"'
        result = subprocess.run(cmdline, capture_output=True, timeout=45,
                                env=self.env, cwd=self.root)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(b'Process exit code: 0 (0x00000000)', result.stdout)
        self.assertIn('Application exited: status=0', self.read_log())

    def test_launcher_reports_loader_failure_without_application_entry(self):
        package = self.root / 'incomplete package Ω'; package.mkdir()
        shutil.copy2(APP, package / 'ccurate.exe')
        for name in ('CCurate-Debug.cmd', 'CCurate-Debug.ps1'):
            shutil.copy2(SCRIPTS / name, package / name)
        # This intentionally incomplete fixture has no runtime DLLs, and PATH
        # contains only Windows directories. Never modify the built package.
        env = dict(self.env, PATH=os.environ['SystemRoot'] + '\\System32;' + os.environ['SystemRoot'])
        powershell = Path(os.environ['SystemRoot']) / 'System32/WindowsPowerShell/v1.0/powershell.exe'
        result = subprocess.run([str(powershell), '-NoLogo', '-NoProfile', '-ExecutionPolicy', 'Bypass',
                                 '-File', str(package / 'CCurate-Debug.ps1'), '-NoPause'],
                                capture_output=True, timeout=45, env=env, cwd=self.root)
        self.assertEqual(result.returncode & 0xffffffff, 0xC0000135, result.stdout + result.stderr)
        transcripts = list(self.logs.glob('launcher-*.log'))
        self.assertEqual(len(transcripts), 1)
        text = transcripts[0].read_text(encoding='utf-8-sig')
        self.assertIn('0xC0000135', text)
        self.assertIn('required DLL', text)
        self.assertIn('No application startup log was created', text)
        self.assertFalse(list(self.logs.glob('startup-*.log')))


if __name__ == '__main__':
    unittest.main(verbosity=2)
