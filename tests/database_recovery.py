#!/usr/bin/env python3
"""Hard-kill/WAL recovery and lock regressions on Linux and native Windows."""
from contextlib import closing
from pathlib import Path
import hashlib
import sqlite3
import subprocess
import sys
import tempfile
import time
import unittest

APP, DRIVER = (str(Path(p).resolve()) for p in sys.argv[1:3])
del sys.argv[1:3]


class DatabaseRecovery(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='ccurate-recovery-')
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.src, self.dst = self.root / 'source', self.root / 'destination'
        self.src.mkdir(); self.dst.mkdir()
        self.state = self.dst / '.ccurate'
        self.payload = b'retained history survives a killed scanner\n'
        (self.src / 'original.txt').write_bytes(self.payload)
        (self.src / 'duplicate.txt').write_bytes(self.payload)

    def command(self, *args):
        return [APP, '--cli', str(self.src), str(self.dst), *args]

    def run_app(self, *args):
        result = subprocess.run(self.command(*args), capture_output=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stderr.decode(errors='replace'))
        return result

    def test_kill_scan_reopen_without_deleting_state(self):
        self.run_app('scan')
        self.run_app('copy', 'original.txt', 'Curated')
        self.run_app('review', 'original.txt')
        retained = (self.state / 'archive.jsonl').read_bytes()
        # Keep the scanner hashing until the parent kills it; all data are fixtures.
        large = self.src / 'large.bin'
        with large.open('wb') as stream:
            stream.truncate(512 * 1024 * 1024)
        log = self.state / 'activity.log'
        offset = log.stat().st_size
        with (self.root / 'killed-scan.log').open('wb') as output:
            process = subprocess.Popen(self.command('scan'), stdout=output, stderr=output)
            try:
                deadline = time.monotonic() + 30
                while time.monotonic() < deadline:
                    self.assertIsNone(process.poll(), 'scan exited before the kill test')
                    with log.open('rb') as stream:
                        stream.seek(offset)
                        if b' | scan.enumerated | ' in stream.read():
                            break
                    time.sleep(0.002)
                else:
                    self.fail('scan never reached hashing')
                # An actually live instance must still be refused.
                second = subprocess.run(self.command('scan'), capture_output=True, timeout=10)
                self.assertNotEqual(second.returncode, 0)
                self.assertIn(b'active session', second.stderr)
                self.assertIsNone(process.poll())
            finally:
                if process.poll() is None:
                    process.kill()
                process.wait(timeout=10)
        self.assertTrue((self.state / 'session.lock').exists())
        self.assertGreater((self.state / 'catalog.sqlite-wal').stat().st_size, 0)
        self.assertEqual((self.state / 'archive.jsonl').read_bytes(), retained)
        # Do not open SQLite here: the application must be the first WAL recovery reader.
        large.write_bytes(b'changed after interruption\n')
        self.run_app('scan')
        self.run_app('audit')
        self.assertEqual((self.dst / 'Curated/original.txt').read_bytes(), self.payload)
        self.assertIn('scan.recovery', log.read_text(errors='replace'))
        with closing(sqlite3.connect(self.state / 'catalog.sqlite')) as db:
            self.assertEqual(db.execute('PRAGMA integrity_check').fetchall(), [('ok',)])
            self.assertEqual(db.execute("SELECT value FROM settings WHERE key='scan_complete'").fetchone(), ('1',))
            self.assertEqual(db.execute("SELECT reviewed FROM entries WHERE path='original.txt'").fetchone(), (1,))
            self.assertEqual(db.execute('SELECT COUNT(*) FROM discoveries').fetchone(), (0,))
            self.assertEqual(db.execute('SELECT COUNT(*) FROM versions WHERE accepted=1 AND hash=?',
                                        (hashlib.sha512(self.payload).hexdigest(),)).fetchone(), (2,))

    def test_commit_and_cancellation_faults(self):
        for i in range(512):
            (self.src / f'file-{i:04d}').touch()
        result = subprocess.run([DRIVER, str(self.src), str(self.dst)], capture_output=True, timeout=60)
        self.assertEqual(result.returncode, 0, (result.stdout + result.stderr).decode(errors='replace'))
        print(result.stdout.decode(errors='replace'), end='')

    def test_startup_lock_error_is_logged_and_retry_works(self):
        self.run_app('scan')
        log = self.state / 'activity.log'
        offset = log.stat().st_size
        with closing(sqlite3.connect(self.state / 'catalog.sqlite')) as writer:
            writer.execute('BEGIN IMMEDIATE')
            result = subprocess.run(self.command('scan'), capture_output=True, timeout=25)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(b'SQLite busy [5/5]', result.stderr)
            with log.open('rb') as stream:
                stream.seek(offset)
                self.assertIn(b'SQLite busy [5/5]', stream.read())
            writer.rollback()
        self.run_app('scan')


if __name__ == '__main__':
    unittest.main()
