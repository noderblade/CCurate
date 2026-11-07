#!/usr/bin/env python3
"""Native Windows black-box checks. Every filesystem mutation uses a fixture."""
import base64
import hashlib
import json
import msvcrt
import os
from pathlib import Path
import shutil
import sqlite3
import subprocess
import sys
import tempfile
import unittest

APP = str(Path(sys.argv.pop(1)).resolve())


class WindowsCuration(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='ccurate-windows-')
        self.root = Path(self.temp.name)
        self.src, self.dst = self.root / 'old', self.root / 'new'
        (self.src / 'A' / 'empty').mkdir(parents=True)
        (self.src / 'B').mkdir()
        self.dst.mkdir()
        self.data = b'precious\x00data\r\n\x1a\xff'
        (self.src / 'A/one.txt').write_bytes(self.data)
        (self.src / 'B/copy.bin').write_bytes(self.data)
        (self.src / '.hidden').write_bytes(b'hidden data')
        self.run_app('scan')

    def tearDown(self):
        self.temp.cleanup()

    def run_app(self, *args, ok=True, source=None, dest=None):
        result = subprocess.run([APP, '--cli', str(source or self.src), str(dest or self.dst), *args],
                                capture_output=True, timeout=60)
        if ok:
            if result.returncode:
                state = Path(dest or self.dst) / '.ccurate'
                details = (result.stdout + result.stderr).decode('utf-8', 'replace')
                if (state / 'activity.log').exists():
                    details += '\n' + (state / 'activity.log').read_text(errors='replace')[-12000:]
                self.fail(details)
        else:
            self.assertNotEqual(result.returncode, 0, result.stdout.decode('utf-8', 'replace'))
        return result

    def sql(self, query, params=()):
        with sqlite3.connect(self.dst / '.ccurate/catalog.sqlite') as db:
            return db.execute(query, params).fetchall()

    def manifest(self):
        return [json.loads(line) for line in (self.dst / '.ccurate/archive.jsonl').read_text(encoding='utf-8').splitlines()]

    def test_binary_content_deduplication_and_history(self):
        self.run_app('copy', 'A/one.txt', 'Curated')
        self.assertEqual((self.dst / 'Curated/one.txt').read_bytes(), self.data)
        self.assertEqual(self.sql("SELECT COUNT(*) FROM versions WHERE accepted=1 AND hash IS NOT NULL"), [(2,)])
        self.assertEqual(self.sql('SELECT COUNT(*) FROM objects WHERE state=\'verified\''), [(1,)])
        self.run_app('copy', 'B/copy.bin', 'Elsewhere')
        self.assertFalse((self.dst / 'Elsewhere/copy.bin').exists())
        self.run_app('audit')
        self.assertEqual((self.src / 'A/one.txt').read_bytes(), self.data)

    def test_unicode_roots_and_filenames(self):
        source = self.root / 'źródło 中文 🚀'
        destination = self.root / 'cel Ω'
        source.mkdir(); destination.mkdir()
        name = 'zażółć_日本語_🗃.txt'
        (source / name).write_bytes(self.data)
        self.run_app('scan', source=source, dest=destination)
        self.run_app('copy', name, 'Archiwum 東京', source=source, dest=destination)
        self.assertEqual((destination / 'Archiwum 東京' / name).read_bytes(), self.data)
        self.run_app('audit', source=source, dest=destination)

    def test_hidden_and_system_attributes_are_not_filters(self):
        hidden = self.src / 'system-file.txt'
        hidden.write_bytes(b'system fixture')
        import ctypes
        self.assertTrue(ctypes.windll.kernel32.SetFileAttributesW(str(hidden), 0x2 | 0x4))
        self.run_app('scan')
        self.assertEqual(self.sql("SELECT present FROM entries WHERE path='system-file.txt'"), [(1,)])
        self.run_app('copy', 'system-file.txt', '')
        self.assertEqual((self.dst / 'system-file.txt').read_bytes(), b'system fixture')

    def test_long_paths(self):
        path = self.src
        parts = ['folder-' + str(i) + '-' + 'x' * 42 for i in range(7)]
        for part in parts:
            path /= part
        path.mkdir(parents=True)
        (path / 'long.txt').write_bytes(self.data)
        self.run_app('scan')
        self.run_app('copy', '/'.join(parts + ['long.txt']), '/'.join(parts))
        self.assertEqual((self.dst.joinpath(*parts) / 'long.txt').read_bytes(), self.data)

    def test_case_collision_and_path_escape_refused(self):
        (self.dst / 'ONE.TXT').write_bytes(b'keep this')
        self.run_app('copy', 'A/one.txt', '', ok=False)
        self.assertEqual((self.dst / 'ONE.TXT').read_bytes(), b'keep this')
        for path in ('../outside', '..\\outside', 'C:/outside', 'folder:stream'):
            self.run_app('mkdir', path, ok=False)
        self.assertFalse((self.root / 'outside').exists())

    def test_invalid_root_pairs_never_create_session(self):
        fresh = self.root / 'untouched'; fresh.mkdir()
        for source, destination in ((fresh, fresh), (fresh, str(fresh).upper()),
                                    (fresh, self.root)):
            self.run_app('scan', source=source, dest=destination, ok=False)
            self.assertFalse((fresh / '.ccurate').exists())
            self.assertFalse((self.root / '.ccurate').exists())

    def test_audit_detects_destination_corruption(self):
        self.run_app('copy', 'A/one.txt', '')
        (self.dst / 'one.txt').write_bytes(b'damaged destination bytes')
        self.run_app('audit', ok=False)
        self.assertNotEqual(self.sql('SELECT state FROM objects WHERE dest=\'one.txt\''), [('verified',)])
        self.assertEqual((self.src / 'A/one.txt').read_bytes(), self.data)

    def test_case_sensitive_directory_is_not_misidentified(self):
        directory = self.src / 'sensitive'; directory.mkdir()
        result = subprocess.run(['fsutil', 'file', 'setCaseSensitiveInfo', str(directory), 'enable'],
                                capture_output=True)
        if result.returncode:
            self.skipTest('This Windows host cannot create a case-sensitive NTFS fixture')
        (directory / 'a.txt').write_bytes(b'lower case content')
        (directory / 'A.txt').write_bytes(b'UPPER case content')
        self.run_app('scan', ok=False)
        self.assertTrue(self.sql("SELECT error FROM entries WHERE path='sensitive'")[0][0])
        self.assertEqual(self.sql("SELECT COUNT(*) FROM entries WHERE path LIKE 'sensitive/%'"), [(0,)])
        self.assertEqual((directory / 'a.txt').read_bytes(), b'lower case content')
        self.assertEqual((directory / 'A.txt').read_bytes(), b'UPPER case content')

    def test_session_lock(self):
        with (self.dst / '.ccurate/session.lock').open('r+b', buffering=0) as lock:
            msvcrt.locking(lock.fileno(), msvcrt.LK_NBLCK, 1)
            try:
                result = self.run_app('scan', ok=False)
                self.assertIn(b'already has an active session', result.stderr)
            finally:
                lock.seek(0); msvcrt.locking(lock.fileno(), msvcrt.LK_UNLCK, 1)
        self.run_app('scan')

    def test_source_change_creates_retained_version(self):
        self.run_app('copy', 'A/one.txt', 'Original')
        (self.src / 'A/one.txt').write_bytes(b'changed data')
        self.run_app('scan')
        self.assertGreaterEqual(self.sql("SELECT COUNT(*) FROM versions v JOIN entries e ON e.id=v.eid WHERE e.path='A/one.txt'")[0][0], 2)
        self.assertEqual((self.dst / 'Original/one.txt').read_bytes(), self.data)
        self.run_app('copy', 'A/one.txt', 'Updated')
        self.run_app('audit')

    def test_review_reject_restore_complete_directory(self):
        self.run_app('review', 'A/one.txt')
        self.assertEqual(self.sql("SELECT reviewed FROM entries WHERE path='A/one.txt'"), [(1,)])
        self.run_app('reject', 'B/copy.bin')
        self.run_app('copy', 'A', '', '--complete')
        entries = {r.get('path') for r in self.manifest() if r['type'] == 'entry'}
        self.assertIn('A/empty', entries)
        self.assertNotIn('B/copy.bin', entries)
        self.run_app('restore', 'B/copy.bin')
        self.run_app('copy', 'B/copy.bin', '')
        self.assertTrue((self.src / 'B/copy.bin').exists())

    def test_move_updates_history(self):
        self.run_app('copy', 'A/one.txt', 'First')
        self.run_app('mkdir', 'Final')
        self.run_app('move', 'First/one.txt', 'Final/renamed.txt')
        self.assertEqual(self.sql('SELECT dest FROM objects WHERE state=\'verified\''), [('Final/renamed.txt',)])
        self.run_app('audit')

    def test_recovery_after_publication(self):
        sha = hashlib.sha512(self.data).hexdigest()
        (self.dst / 'recovered.txt').write_bytes(self.data)
        self.sql("INSERT INTO operations(kind,src,dst,temp,hash) VALUES('copy','A/one.txt','recovered.txt','missing.part',?)", (sha,))
        self.run_app('export')
        self.assertEqual(self.sql('SELECT dest,state FROM objects WHERE hash=?', (sha,)), [('recovered.txt', 'verified')])

    def test_jsonl_rebuild_without_source(self):
        self.run_app('copy', 'A', '', '--complete')
        fresh = self.root / 'reconstructed'; fresh.mkdir()
        shutil.copytree(self.dst / 'A', fresh / 'A')
        self.run_app('import', str(self.dst / '.ccurate/archive.jsonl'), source='-', dest=fresh)
        self.run_app('audit', source='-', dest=fresh)
        self.assertIn(b'B/copy.bin', self.run_app('list', 'B', '--history', source='-', dest=fresh).stdout)

    def test_historical_posix_names_are_not_windows_paths(self):
        self.run_app('copy', 'A', '', '--complete')
        records = self.manifest()
        for record in records:
            if record['type'] == 'entry' and record.get('path', '').startswith('A'):
                path = record['path'].replace('A', 'C:', 1)
                record['path'] = path
                record['path_b64'] = base64.b64encode(path.encode()).decode()
        manifest = self.root / 'foreign.jsonl'
        manifest.write_text(''.join(json.dumps(r) + '\n' for r in records), encoding='utf-8')
        fresh = self.root / 'foreign'; fresh.mkdir()
        shutil.copytree(self.dst / 'A', fresh / 'A')
        self.run_app('import', str(manifest), source='-', dest=fresh)
        listing = self.run_app('list', 'C:', '--history', source='-', dest=fresh).stdout
        self.assertIn(b'C:/one.txt', listing)
        self.assertIn(b'C:/empty', listing)
        self.run_app('audit', source='-', dest=fresh)

    def test_native_metadata_is_retained(self):
        self.run_app('copy', 'A/one.txt', '')
        entry = next(r for r in self.manifest() if r.get('path') == 'A/one.txt' and r['type'] == 'entry')
        names = {base64.b64decode(x['name_b64']).decode() for x in entry['xattrs']}
        self.assertIn('windows.security_descriptor_sddl', names)
        self.assertIn('windows.file_attributes', names)

    def test_named_stream_blocks_false_secured_status(self):
        path = self.src / 'streams.txt'
        path.write_bytes(b'main content')
        with open(str(path) + ':important', 'wb') as stream:
            stream.write(b'additional important content')
        self.run_app('scan', ok=False)
        self.assertIn('Alternate data stream', self.sql("SELECT error FROM entries WHERE path='streams.txt'")[0][0])
        self.run_app('copy', 'streams.txt', '', ok=False)
        self.assertFalse((self.dst / 'streams.txt').exists())

    def test_junctions_are_not_followed(self):
        outside = self.root / 'outside'; outside.mkdir()
        (outside / 'precious.txt').write_bytes(b'untouched')
        for link in (self.src / 'junction', self.dst / 'escape'):
            subprocess.run(['cmd', '/c', 'mklink', '/J', str(link), str(outside)], check=True, capture_output=True)
        try:
            self.run_app('scan')
            self.assertEqual(self.sql("SELECT kind FROM entries WHERE path='junction'"), [('symlink',)])
            self.assertEqual(self.sql("SELECT COUNT(*) FROM entries WHERE path LIKE 'junction/%'"), [(0,)])
            self.run_app('copy', 'A/one.txt', 'escape', ok=False)
            self.assertFalse((outside / 'one.txt').exists())
            self.assertEqual((outside / 'precious.txt').read_bytes(), b'untouched')
        finally:
            os.rmdir(self.src / 'junction'); os.rmdir(self.dst / 'escape')


if __name__ == '__main__':
    unittest.main(verbosity=2)
