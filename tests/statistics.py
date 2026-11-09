#!/usr/bin/env python3
"""Space estimates use current eligible contents, not historical or pending hashes."""
from pathlib import Path
import re
import sqlite3
import subprocess
import sys
import tempfile
import unittest

APP = str(Path(sys.argv.pop(1)).resolve())


class Statistics(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='ccurate-stats-')
        self.base = Path(self.temp.name)
        self.source, self.destination = self.base / 'source', self.base / 'destination'
        self.source.mkdir(); self.destination.mkdir()
        (self.source / 'one').write_bytes(b'a' * 100)
        (self.source / 'duplicate').write_bytes(b'a' * 100)
        (self.source / 'other').write_bytes(b'b' * 31)
        (self.source / 'empty-a').touch(); (self.source / 'empty-b').touch()
        self.run_app('scan')

    def tearDown(self):
        self.temp.cleanup()

    def run_app(self, *args, ok=True):
        result = subprocess.run([APP, '--cli', str(self.source), str(self.destination), *args],
                                capture_output=True, text=True, timeout=30)
        if ok:
            self.assertEqual(result.returncode, 0, result.stderr)
        return result.stdout

    def sql(self, statement):
        with sqlite3.connect(self.destination / '.ccurate/catalog.sqlite') as db:
            db.executescript(statement)

    def check(self, raw, unique, saved, needed, files, empty=2, unknown=0, partial=False):
        output = self.run_app('balance')
        for label, value in [('Before deduplication', raw), ('After deduplication', unique),
                             ('Duplicate data saved', saved), ('Additional data still to secure', needed)]:
            match = re.search(re.escape(label) + r': .*?\((\d+) B\)', output)
            self.assertIsNotNone(match, output)
            self.assertEqual(int(match[1]), value, output)
        self.assertRegex(output, rf'Before deduplication: .*?, {files} files\n')
        self.assertIn(f'Empty files kept individually: {empty} ', output)
        self.assertIn(f'Unscanned / changing files: {unknown} ', output)
        self.assertEqual('[KNOWN CONTENT ONLY]' in output, partial or unknown > 0, output)
        return output

    def test_known_contents_empty_files_and_verified_copy(self):
        text = self.check(231, 131, 100, 131, 5)
        self.assertIn('Redundant nonempty copies: 1', text)
        self.run_app('copy', 'one', 'Curated')
        self.run_app('copy', 'empty-a', 'Curated')
        self.check(231, 131, 100, 31, 5)

    def test_reject_one_occurrence_then_entire_group(self):
        self.run_app('reject', 'one')
        self.check(131, 131, 0, 131, 4)
        self.run_app('reject', 'duplicate')
        self.check(31, 31, 0, 31, 3)
        self.run_app('restore', 'one')
        self.check(131, 131, 0, 131, 4)

    def test_retained_old_version_and_missing_path(self):
        self.run_app('copy', 'one', 'Curated')
        (self.source / 'one').write_bytes(b'changed' * 11)
        self.run_app('scan')
        self.check(208, 208, 0, 108, 5)
        (self.source / 'duplicate').unlink()
        self.run_app('scan')
        self.check(108, 108, 0, 108, 4)

    def test_pending_and_unscanned_never_reuse_stale_hashes(self):
        self.run_app('copy', 'one', 'Curated')
        self.sql("INSERT INTO discoveries(eid,size) SELECT id,143 FROM entries WHERE path='one';")
        self.check(274, 131, 0, 31, 5, unknown=1)
        self.sql("UPDATE entries SET error='UNSCANNED: file is open for writing' WHERE path='duplicate';")
        self.check(274, 31, 0, 31, 5, unknown=2)

    def test_failed_traversal_and_new_unhashed_file(self):
        self.sql("UPDATE settings SET value='0' WHERE key='scan_complete';")
        self.check(231, 131, 100, 131, 5, partial=True)
        self.sql("INSERT INTO entries(path,parent,name,kind,error) VALUES('new','','new','file','UNSCANNED: busy');"
                 "INSERT INTO discoveries(eid,size) SELECT id,17 FROM entries WHERE path='new';")
        self.check(248, 131, 100, 131, 6, unknown=1, partial=True)

    def test_corrupt_target_is_counted_as_still_needed(self):
        self.run_app('copy', 'one', 'Curated')
        (self.destination / 'Curated' / 'one').write_bytes(b'broken')
        self.run_app('audit', ok=False)
        self.check(231, 131, 100, 131, 5)

    def test_all_rejected_and_empty_plan(self):
        self.run_app('reject', '')
        self.check(0, 0, 0, 0, 0, empty=0)


if __name__ == '__main__':
    unittest.main(verbosity=2)
