#!/usr/bin/env python3
"""Destination removal against disposable files, on Linux and native Windows."""
from contextlib import closing
import hashlib
import json
import os
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile
import unittest

APP = str(Path(sys.argv.pop(1)).resolve())


class DestinationRemoval(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='ccurate-remove-')
        self.root = Path(self.temporary.name)
        self.source, self.destination = self.root / 'old', self.root / 'new'
        self.source.mkdir(); self.destination.mkdir()
        self.payload = b'Precious original bytes\x00\xff\n'
        (self.source / 'original.txt').write_bytes(self.payload)
        (self.source / 'duplicate.txt').write_bytes(self.payload)
        for name in ('empty-one', 'empty-two'):
            (self.source / name).touch()
        self.digest = hashlib.sha512(self.payload).hexdigest()
        self.state = self.destination / '.ccurate'
        self.run_app('scan')
        self.run_app('copy', 'original.txt', 'Curated')
        self.target = self.destination / 'Curated/original.txt'

    def tearDown(self):
        self.temporary.cleanup()

    def run_app(self, *args, ok=True, source=None, dest=None):
        result = subprocess.run([APP, '--cli', str(source or self.source),
                                 str(dest or self.destination), *args], capture_output=True,
                                timeout=45)
        message = (result.stdout + result.stderr).decode(errors='replace')
        self.assertEqual(result.returncode == 0, ok, message)
        return message

    def sql(self, query, args=()):
        # sqlite3's transaction context alone does not close handles on Windows.
        with closing(sqlite3.connect(self.state / 'catalog.sqlite')) as db, db:
            return db.execute(query, args).fetchall()

    def manifest(self):
        return [json.loads(line) for line in (self.state / 'archive.jsonl').read_text().splitlines()]

    def remove(self, path='Curated/original.txt', **kw):
        return self.run_app('remove', path, '--confirm', **kw)

    def assert_kept(self):
        self.assertEqual(self.target.read_bytes(), self.payload)
        self.assertFalse(list(self.destination.rglob('.ccurate-remove-*')))

    def assert_detached(self):
        self.assertFalse(self.target.exists())
        self.assertEqual(self.sql('SELECT dest,state,verified FROM objects WHERE hash=?',
                                  (self.digest,)), [(None, 'missing', 0)])
        self.assertEqual(self.sql("SELECT COUNT(*) FROM operations WHERE kind='remove' "
                                  "AND state='pending'"), [(0,)])
        self.assertEqual(self.sql("SELECT COUNT(*) FROM events WHERE op='remove'"), [(1,)])
        self.assertFalse(any(row['type'] == 'object' and row['sha512'] == self.digest
                             for row in self.manifest()))

    def test_remove_recopies_and_unsecures_all_occurrences(self):
        self.run_app('review', 'original.txt')
        flags = self.sql('SELECT path,reviewed,discarded FROM entries ORDER BY path')
        history = [row for row in self.manifest() if row['type'] == 'entry']
        self.assertNotIn('original.txt', self.run_app('list', '', '--unsecured'))
        self.remove()
        self.assert_detached()
        self.assertEqual(flags, self.sql('SELECT path,reviewed,discarded FROM entries ORDER BY path'))
        self.assertEqual(history, [row for row in self.manifest() if row['type'] == 'entry'])
        remaining = self.run_app('list', '', '--unsecured')
        self.assertIn('original.txt', remaining)
        self.assertIn('duplicate.txt', remaining)
        details = self.run_app('info', 'original.txt')
        self.assertIn('No destination copy', details)
        self.assertNotIn('[OK]', details)
        balance = self.run_app('balance')
        self.assertIn(f'Additional data still to secure: {len(self.payload)} B ({len(self.payload)} B)', balance)
        self.assertIn('Secured: 0\n', balance)
        self.assertEqual((self.source / 'original.txt').read_bytes(), self.payload)
        self.assertEqual((self.source / 'duplicate.txt').read_bytes(), self.payload)
        events = [row for row in self.manifest() if row['type'] == 'event' and
                  row['operation'] == 'remove']
        self.assertEqual(events[0]['from'], 'Curated/original.txt')
        self.assertEqual(events[0]['to'], '')
        self.assertIn('remove.complete', (self.state / 'activity.log').read_text())
        self.run_app('copy', 'duplicate.txt', 'Better place')
        self.assertEqual((self.destination / 'Better place/duplicate.txt').read_bytes(), self.payload)
        self.assertEqual(self.sql('SELECT dest,state FROM objects WHERE hash=?', (self.digest,)),
                         [('Better place/duplicate.txt', 'verified')])
        self.assertNotIn('original.txt', self.run_app('list', '', '--unsecured'))
        self.run_app('audit')

    def test_requires_explicit_cli_confirmation(self):
        for args in ((), ('Curated/original.txt',), ('Curated/original.txt', '--force')):
            self.run_app('remove', *args, ok=False)
        self.assert_kept()

    def test_reject_and_review_flags_survive(self):
        self.run_app('review', 'duplicate.txt')
        self.run_app('reject', 'duplicate.txt')
        before = self.sql('SELECT path,reviewed,discarded FROM entries ORDER BY path')
        self.remove()
        self.assertEqual(before, self.sql('SELECT path,reviewed,discarded FROM entries ORDER BY path'))
        self.assertNotIn('duplicate.txt', self.run_app('list', '', '--unsecured'))
        self.run_app('restore', 'duplicate.txt')
        self.assertIn('duplicate.txt', self.run_app('list', '', '--unsecured'))

    def test_source_missing_or_changed_refuses_removal(self):
        (self.source / 'duplicate.txt').unlink()
        original = self.source / 'original.txt'
        before = original.stat()
        original.write_bytes(b'X' * len(self.payload))
        os.utime(original, ns=(before.st_atime_ns, before.st_mtime_ns))
        self.assertIn('no stable source copy', self.remove(ok=False))
        self.assert_kept()
        original.unlink()
        self.remove(ok=False)
        self.assert_kept()

    def test_uses_verified_duplicate_when_original_changes(self):
        (self.source / 'original.txt').write_bytes(b'new version')
        self.remove()
        self.assert_detached()
        self.assertEqual((self.source / 'original.txt').read_bytes(), b'new version')
        self.assertEqual((self.source / 'duplicate.txt').read_bytes(), self.payload)

    def test_old_archived_version_is_protected(self):
        (self.source / 'original.txt').write_bytes(b'new original version')
        (self.source / 'duplicate.txt').write_bytes(b'new duplicate version')
        self.run_app('scan')
        self.run_app('copy', 'original.txt', 'Updated')
        self.assertIn('no stable source copy', self.remove(ok=False))
        self.assert_kept()
        self.remove('Updated/original.txt')
        self.assert_kept()

    def test_destination_edits_are_protected(self):
        self.target.write_bytes(b'edited on destination')
        self.assertIn('destination content changed', self.remove(ok=False))
        self.assertEqual(self.target.read_bytes(), b'edited on destination')
        self.assertEqual((self.source / 'original.txt').read_bytes(), self.payload)

    def test_empty_files_are_independent(self):
        self.run_app('copy', 'empty-one', 'Empty')
        self.run_app('copy', 'empty-two', 'Empty')
        self.remove('Empty/empty-one')
        self.assertFalse((self.destination / 'Empty/empty-one').exists())
        self.assertTrue((self.destination / 'Empty/empty-two').exists())
        self.assertEqual(self.sql('SELECT dest FROM empty_copies'), [('Empty/empty-two',)])
        remaining = self.run_app('list', '', '--unsecured')
        self.assertIn('empty-one', remaining)
        self.assertNotIn('empty-two', remaining)
        self.run_app('copy', 'empty-one', 'New empty')
        self.assertTrue((self.destination / 'New empty/empty-one').exists())
        self.assert_kept()

    def test_wrong_paths_and_nonindexed_files_refused(self):
        outsider = self.root / 'outside.txt'
        outsider.write_bytes(self.payload)
        (self.destination / 'manual.txt').write_bytes(self.payload)
        for path in ('', '.', '..', '../outside.txt', str(outsider), 'Curated',
                     '.ccurate', '.ccurate/catalog.sqlite', 'manual.txt'):
            self.remove(path, ok=False)
        self.assertEqual(outsider.read_bytes(), self.payload)
        self.assertEqual((self.destination / 'manual.txt').read_bytes(), self.payload)
        self.assert_kept()

    def test_offline_removal_refused(self):
        self.assertIn('live source', self.remove(source='-', ok=False))
        self.assert_kept()

    def test_open_writer_is_not_a_safe_source(self):
        (self.source / 'duplicate.txt').unlink()
        with (self.source / 'original.txt').open('r+b'):
            self.remove(ok=False)
        self.assert_kept()
        self.remove()
        self.assert_detached()

    @unittest.skipIf(os.name == 'nt', 'symlinks require optional Windows privileges')
    def test_symlinks_are_never_followed(self):
        self.target.unlink()
        self.target.symlink_to(self.source / 'original.txt')
        self.remove(ok=False)
        self.assertTrue(self.target.is_symlink())
        self.target.unlink()
        self.target.write_bytes(self.payload)
        for name in ('original.txt', 'duplicate.txt'):
            (self.source / name).unlink()
            (self.source / name).symlink_to(self.target)
        self.remove(ok=False)
        self.assert_kept()

    def pending_removal(self, stage):
        temp = 'Curated/.ccurate-remove-100-fixture.pending'
        self.sql("INSERT INTO operations(kind,src,dst,temp,hash) "
                 "VALUES('remove','original.txt','Curated/original.txt',?,?)", (temp, self.digest))
        self.sql("UPDATE objects SET state='unchecked',verified=0 WHERE hash=?", (self.digest,))
        staged = self.destination / temp
        if stage:
            self.target.rename(staged)
        return staged

    def test_recovery_before_rename_preserves_file(self):
        self.pending_removal(False)
        self.run_app('stats', source='-')
        self.assert_kept()
        self.assertEqual(self.sql('SELECT state FROM objects WHERE hash=?', (self.digest,)),
                         [('unchecked',)])
        self.run_app('audit')

    def test_recovery_restores_staged_copy_even_without_source(self):
        staged = self.pending_removal(True)
        (self.source / 'original.txt').unlink()
        (self.source / 'duplicate.txt').unlink()
        self.run_app('stats', source='-')
        self.assertFalse(staged.exists())
        self.assert_kept()
        self.assertEqual(self.sql("SELECT state FROM operations WHERE kind='remove'"),
                         [('interrupted',)])
        self.remove(ok=False)

    def test_recovery_after_unlink_repairs_index_once(self):
        self.pending_removal(True).unlink()
        self.run_app('stats', source='-')
        self.assert_detached()
        self.run_app('stats', source='-')
        self.assert_detached()

    def test_restart_retries_jsonl_after_committed_removal(self):
        before = (self.state / 'archive.jsonl').read_bytes()
        self.remove()
        self.assert_detached()
        # Simulate a crash after metadata commit, before the atomic JSONL replacement.
        self.sql("UPDATE operations SET state='remove_export' WHERE kind='remove'")
        (self.state / 'archive.jsonl').write_bytes(before)
        self.run_app('stats', source='-')
        self.assert_detached()
        self.assertEqual(self.sql("SELECT state FROM operations WHERE kind='remove'"), [('done',)])
        self.run_app('stats', source='-')
        self.assert_detached()

    def test_recovery_collision_preserves_both_files(self):
        staged = self.pending_removal(True)
        self.target.write_bytes(b'new unrelated file')
        self.run_app('stats', source='-')
        self.assertEqual(self.target.read_bytes(), b'new unrelated file')
        self.assertEqual(staged.read_bytes(), self.payload)
        self.assertEqual(self.sql('SELECT dest,state FROM objects WHERE hash=?', (self.digest,)),
                         [(staged.relative_to(self.destination).as_posix(), 'unchecked')])
        self.run_app('audit')
        self.assertEqual(self.sql('SELECT state FROM objects WHERE hash=?', (self.digest,)),
                         [('verified',)])

    def test_manifest_round_trip_preserves_removed_history(self):
        self.remove()
        fresh = self.root / 'rebuilt'
        fresh.mkdir()
        self.run_app('import', str(self.state / 'archive.jsonl'), source='-', dest=fresh)
        history = self.run_app('list', '', '--history', source='-', dest=fresh)
        self.assertIn('original.txt', history)
        self.assertIn('duplicate.txt', history)
        self.assertNotIn('verified', history)


if __name__ == '__main__':
    unittest.main()
