#!/usr/bin/env python3
"""Lazy refresh against real files: scope, history, absence, interruption, copies."""
import hashlib
import os
from pathlib import Path
import shutil
import sqlite3
import subprocess
import sys
import tempfile
import unittest

APP, DRIVER = (str(Path(sys.argv.pop(1)).resolve()) for _ in range(2))


class LazyRefresh(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='ccurate-lazy-')
        self.root = Path(self.temp.name)
        self.src, self.dst = self.root / 'source', self.root / 'destination'
        self.src.mkdir(); self.dst.mkdir()
        (self.src / 'A/deep').mkdir(parents=True)
        (self.src / 'B').mkdir()
        (self.src / 'A/one').write_bytes(b'original content')
        (self.src / 'B/copy').write_bytes(b'original content')
        (self.src / 'A/deep/untouched').write_bytes(b'deep')
        self.run_app('scan')
        self.baseline = self.sql("SELECT value FROM settings WHERE key='scan_started'")[0][0]

    def tearDown(self):
        self.temp.cleanup()

    def run_app(self, *args, ok=True):
        r = subprocess.run([APP, '--cli', str(self.src), str(self.dst), *args],
                           capture_output=True, text=True, timeout=30)
        if ok:
            self.assertEqual(r.returncode, 0, r.stderr)
        else:
            self.assertNotEqual(r.returncode, 0)
        return r.stdout

    def driver(self, command, path=''):
        r = subprocess.run([DRIVER, str(self.src), str(self.dst), command, path],
                           capture_output=True, text=True, timeout=30)
        self.assertEqual(r.returncode, 0, r.stderr)
        return r.stdout

    def sql(self, query, args=()):
        with sqlite3.connect(self.dst / '.ccurate/catalog.sqlite') as db:
            return db.execute(query, args).fetchall()

    def content(self, path):
        return self.sql("SELECT v.hash FROM entries e JOIN versions v ON v.id=e.current "
                        "WHERE e.path=?", (path,))[0][0]

    def test_startup_baseline_and_unchanged_visits(self):
        self.assertIn('initial=0', self.driver('initial'))
        self.driver('unchanged', 'A')
        self.assertEqual(self.sql("SELECT value FROM settings WHERE key='scan_started'"),
                         [(self.baseline,)])
        self.sql("UPDATE settings SET value='0' WHERE key='scan_complete'")
        self.assertIn('initial=0', self.driver('initial'))
        shutil.rmtree(self.dst / '.ccurate')
        self.assertIn('initial=1', self.driver('initial'))

    def test_only_direct_children_refreshed(self):
        old = self.content('A/one')
        (self.src / 'A/one').write_bytes(b'changed!')
        (self.src / 'B/copy').write_bytes(b'unvisited sibling')
        (self.src / 'A/deep/new').write_text('unvisited descendant')
        (self.src / 'A/.hidden').write_text('hidden')
        self.run_app('refresh', 'A')
        self.assertNotEqual(self.content('A/one'), old)
        self.assertEqual(self.content('B/copy'), old)
        self.assertFalse(self.sql("SELECT 1 FROM entries WHERE path='A/deep/new'"))
        self.assertTrue(self.content('A/.hidden'))
        self.assertEqual(self.sql('SELECT occurrences FROM hash_groups WHERE hash=?', (old,)), [(1,)])
        self.assertEqual(self.sql("SELECT value FROM settings WHERE key='scan_complete'"), [('1',)])
        self.run_app('refresh', 'A/deep')
        self.assertTrue(self.content('A/deep/new'))

    def test_same_size_and_restored_mtime_still_rehashed(self):
        path = self.src / 'A/one'
        before = path.stat()
        data = b'X' * before.st_size
        path.write_bytes(data)
        os.utime(path, ns=(before.st_atime_ns, before.st_mtime_ns))
        self.run_app('refresh', 'A')
        self.assertEqual(self.content('A/one'), hashlib.sha512(data).hexdigest())
        self.driver('unchanged', 'A')

    def test_deleted_subtree_and_reappearance_preserve_history(self):
        self.run_app('copy', 'A', 'Curated', '--complete')
        retained = self.sql('SELECT id FROM versions WHERE accepted=1')
        shutil.rmtree(self.src / 'A/deep')
        self.run_app('refresh', 'A')
        self.assertEqual(self.sql("SELECT present FROM entries WHERE path LIKE 'A/deep%'"),
                         [(0,), (0,)])
        self.assertTrue(set(retained) <= set(self.sql('SELECT id FROM versions WHERE accepted=1')))
        self.assertTrue((self.dst / 'Curated/A/deep/untouched').exists())
        (self.src / 'A/deep').mkdir()
        (self.src / 'A/deep/untouched').write_bytes(b'deep')
        self.run_app('refresh', 'A')
        self.assertEqual(self.sql("SELECT present FROM entries WHERE path='A/deep/untouched'"), [(0,)])
        self.run_app('refresh', 'A/deep')
        self.assertEqual(self.sql("SELECT present FROM entries WHERE path='A/deep/untouched'"), [(1,)])

    def test_type_change_and_symlink_not_followed(self):
        shutil.rmtree(self.src / 'A/deep')
        (self.src / 'A/deep').symlink_to('../B')
        self.run_app('refresh', 'A')
        self.assertEqual(self.sql("SELECT present FROM entries WHERE path='A/deep/untouched'"), [(0,)])
        self.assertEqual(self.sql("SELECT kind FROM entries WHERE path='A/deep'"), [('symlink',)])
        self.assertFalse(self.sql("SELECT 1 FROM entries WHERE path='A/deep/copy'"))

    def test_failed_listing_does_not_infer_absence(self):
        (self.src / 'A').rename(self.src / 'elsewhere')
        self.run_app('refresh', 'A', ok=False)
        self.assertEqual(self.sql("SELECT present FROM entries WHERE path='A/one'"), [(1,)])

    def test_cancelled_hash_keeps_pending_observation_and_old_version(self):
        self.run_app('copy', 'A/one', 'Curated')
        old = self.content('A/one')
        version = self.sql("SELECT current FROM entries WHERE path='A/one'")
        with (self.src / 'A/one').open('wb') as f:
            f.truncate(2 * 1024**3)
        self.driver('cancel', 'A')
        self.assertEqual(self.content('A/one'), old)
        self.assertEqual(self.sql("SELECT current FROM entries WHERE path='A/one'"), version)
        self.assertTrue(self.sql("SELECT 1 FROM discoveries d JOIN entries e ON e.id=d.eid WHERE e.path='A/one'"))
        self.assertIn('UNSCANNED', self.sql("SELECT error FROM entries WHERE path='A/one'")[0][0])
        self.assertEqual(self.sql('SELECT occurrences FROM hash_groups WHERE hash=?', (old,)), [(1,)])
        (self.src / 'A/one').write_bytes(b'finished later')
        self.run_app('refresh', 'A')
        self.assertEqual(self.content('A/one'), hashlib.sha512(b'finished later').hexdigest())
        self.assertEqual((self.dst / 'Curated/one').read_bytes(), b'original content')

    def test_copy_directory_discovers_unvisited_new_descendants(self):
        (self.src / 'A/deep/new-folder').mkdir()
        (self.src / 'A/deep/new-folder/new-file').write_bytes(b'new valuable content')
        self.driver('copy', 'A')
        self.assertEqual((self.dst / 'Curated/A/deep/new-folder/new-file').read_bytes(),
                         b'new valuable content')
        self.assertIn('A/deep/new-folder/new-file', self.run_app('list', 'A/deep/new-folder', '--history'))

    def test_empty_files_independent_and_open_writer_deferred(self):
        (self.src / 'A/zero1').touch(); (self.src / 'A/zero2').touch()
        with (self.src / 'A/one').open('wb') as writer:
            writer.write(b'active writer'); writer.flush()
            self.run_app('refresh', 'A', ok=False)
            self.assertIn('UNSCANNED', self.sql("SELECT error FROM entries WHERE path='A/one'")[0][0])
        self.run_app('refresh', 'A')
        self.assertEqual(self.sql("SELECT COUNT(*) FROM versions WHERE size=0 AND hash IS NOT NULL"), [(2,)])
        self.assertFalse(self.sql('SELECT 1 FROM hash_groups WHERE hash=?', (hashlib.sha512(b'').hexdigest(),)))
        self.driver('unchanged', 'A')


if __name__ == '__main__':
    unittest.main()
