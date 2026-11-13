#!/usr/bin/env python3
"""Logical usage, subtree unions, incremental updates and offline catalog analysis."""
from contextlib import closing
from pathlib import Path
import hashlib
import json
import os
import random
import sqlite3
import subprocess
import sys
import tempfile
import unittest

APP = str(Path(sys.argv.pop(1)).resolve())
DRIVER = str(Path(sys.argv.pop(1)).resolve())


class Usage(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='ccurate-usage-')
        self.base = Path(self.temporary.name)
        self.source, self.destination = self.base / 'source', self.base / 'destination'
        self.source.mkdir(); self.destination.mkdir()
        self.db = self.destination / '.ccurate/catalog.sqlite'
        self.cache = self.destination / '.ccurate/usage.sqlite'
        for path, data in [('A/one', b'x' * 100), ('A/sub/two', b'x' * 100),
                           ('A-/three', b'x' * 100), ('B/four', b'x' * 100),
                           ('B/unique', b'z' * 31), ('.empty', b''), ('empty-two', b'')]:
            self.write(path, data)
        (self.source / 'empty-directory').mkdir()
        self.app('scan')

    def tearDown(self):
        self.temporary.cleanup()

    def write(self, path, data):
        target = self.source / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)

    def app(self, *command):
        result = subprocess.run([APP, '--cli', str(self.source), str(self.destination), *command],
                                capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stderr)
        return result.stdout

    def sql(self, sql, parameters=()):
        with closing(sqlite3.connect(self.db)) as db, db:
            db.execute(sql, parameters)

    def view(self, path='', keep=0, selection=''):
        result = subprocess.run([DRIVER, str(self.destination), path, str(keep), selection],
                                capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stderr)
        data = json.loads(result.stdout)
        nodes = data['chart']
        for parent in {-1} | {n['parent'] for n in nodes}:
            values = [n['bytes'] for n in nodes if n['parent'] == parent]
            if values:
                expected = data['bytes'] if parent == -1 else nodes[parent]['bytes']
                self.assertEqual(sum(values), expected, (path, parent, nodes))
        return data

    def oracle(self, path='', keep=0):
        with closing(sqlite3.connect(self.db)) as db:
            rows = db.execute("SELECT e.path,e.discarded,e.error,v.id,v.size,v.hash,d.eid,d.size "
                              "FROM entries e LEFT JOIN versions v ON v.id=e.current "
                              "LEFT JOIN discoveries d ON d.eid=e.id WHERE e.present=1 AND e.kind='file'")
            raw = hashed = unknown = unknown_files = files = 0
            unique = {}
            for name, rejected, error, version, size, sha, discovery, observed in rows:
                if path and not name.startswith(path + '/'):
                    continue
                if keep and rejected:
                    continue
                files += 1
                length = max(0, (observed if observed is not None else size) or 0)
                raw += length
                known = version is not None and not error and discovery is None and (
                    size == 0 or sha is not None and len(sha) == 128)
                if not known:
                    unknown += length; unknown_files += 1
                elif size > 0:
                    hashed += size; unique[sha] = size
            return dict(bytes=raw, unique=sum(unique.values()), repeated=hashed-sum(unique.values()),
                        files=files, unknown=unknown, unknown_files=unknown_files)

    def check(self, path='', keep=0):
        actual = self.view(path, keep)
        expected = self.oracle(path, keep)
        self.assertEqual({key: actual[key] for key in expected}, expected, path)
        return actual

    def test_subtree_union_cross_branches_and_lexical_prefixes(self):
        root = self.check()
        self.assertEqual((root['bytes'], root['unique'], root['repeated']), (431, 131, 300))
        for path in ['A', 'A/sub', 'A-', 'B', 'empty-directory']:
            self.check(path)
        self.assertEqual(self.view('A')['unique'], 100)
        self.assertEqual(self.view('A-')['unique'], 100)
        self.assertEqual(self.view('B')['unique'], 131)

    def test_offline_no_filesystem_refresh_or_rehash(self):
        before = self.check()
        self.source.rename(self.base / 'unavailable')
        after = self.check()
        self.assertEqual(before, after)
        with closing(sqlite3.connect(self.cache)) as db:
            tables = {r[0] for r in db.execute("SELECT name FROM sqlite_master WHERE type='table'")}
            self.assertNotIn('versions', tables)
            self.assertNotIn('entries', tables)

    def test_incremental_rejection_and_restoration(self):
        self.check()
        self.app('reject', 'A')
        self.check(); self.check(keep=1); self.check('A', keep=1)
        self.app('restore', 'A/sub/two')
        self.check(keep=1)
        self.assertIn('A', [r['path'] for r in self.view(keep=1)['rows']])
        log = (self.destination / '.ccurate/activity.log').read_text()
        self.assertIn('Incremental logical-usage index ready', log)

    def test_changed_content_missing_path_and_new_directory(self):
        self.check()
        self.write('A/sub/two', b'new-content' * 19)
        self.app('refresh', 'A/sub'); self.check(); self.check('A')
        (self.source / 'B/four').unlink()
        self.app('refresh', 'B'); self.check()
        self.write('new/nested/file', b'new-content' * 19)
        self.app('scan'); self.check(); self.check('new')

    def test_pending_error_and_null_hash_are_not_deduplicated(self):
        self.check()
        self.sql("INSERT INTO discoveries(eid,size) SELECT id,143 FROM entries WHERE path='A/one'")
        self.check(); self.check('A')
        self.sql("UPDATE entries SET error='UNSCANNED: busy' WHERE path='B/four'")
        self.check()
        self.sql("UPDATE versions SET hash=NULL WHERE id=(SELECT current FROM entries WHERE path='B/unique')")
        data = self.check()
        self.assertEqual(data['unknown'], 274)
        self.assertEqual(data['unknown_files'], 3)

    def test_empty_files_and_metadata_only_entries(self):
        if os.name != 'nt':
            os.symlink('A', self.source / 'link-to-directory')
            os.mkfifo(self.source / 'pipe')
            os.link(self.source / 'A/one', self.source / 'hardlink')
            self.app('scan')
        data = self.check()
        names = {r['path']: r for r in data['rows']}
        self.assertEqual(names['.empty']['bytes'], 0)
        self.assertIn('independent metadata entry', self.view(selection='.empty')['details'])
        self.app('copy', '.empty', 'Curated')
        self.assertIn('Destination: /Curated/.empty | verified', self.view(selection='.empty')['details'])
        if os.name != 'nt':
            self.assertEqual(names['link-to-directory']['bytes'], 0)
            self.assertEqual(names['pipe']['bytes'], 0)

    def test_missing_version_is_unverified_not_empty(self):
        self.sql("UPDATE entries SET current=NULL WHERE path='.empty'")
        data = self.check()
        self.assertEqual(data['unknown_files'], 1)
        self.assertIn('UNVERIFIED:', self.view(selection='.empty')['details'])

    def test_occurrences_and_verified_destination(self):
        self.check()
        self.app('copy', 'A/one', 'Curated')
        data = self.view('A', selection='A/one')
        self.assertIn('Destination: /Curated/one | verified', data['details'])
        for name in ['A/one', 'A/sub/two', 'A-/three', 'B/four']:
            self.assertIn('/' + name, data['details'])

    def test_ui_preferences_do_not_invalidate_index(self):
        before = self.check()
        self.sql("INSERT OR REPLACE INTO settings VALUES('ui.theme','3')")
        self.sql("INSERT OR REPLACE INTO settings VALUES('ui.usage','test')")
        after = self.check()
        self.assertEqual(before['revision'], after['revision'])

    def test_cancel_and_pause_resume_complete_generation(self):
        # Enough rows to interrupt real SQL work, not an already finished result.
        with closing(sqlite3.connect(self.db)) as db, db:
            db.execute("""WITH RECURSIVE n(i) AS (VALUES(1) UNION ALL SELECT i+1 FROM n WHERE i<50000)
                INSERT INTO entries(id,path,parent,name,kind,current)
                SELECT 100000+i,printf('B/item-%06d',i),'B',printf('item-%06d',i),'file',100000+i FROM n""")
            db.execute("""INSERT INTO versions(id,eid,mode,size,hash)
                SELECT id,id,33188,100,? FROM entries WHERE id>100000""", (hashlib.sha512(b'x'*100).hexdigest(),))
            db.execute("UPDATE settings SET value='0' WHERE key='hash_groups_ready'")
        for mode in ('cancel', 'pause'):
            if self.cache.exists():
                self.cache.unlink()
            run = subprocess.run([DRIVER, str(self.destination)], capture_output=True, text=True, timeout=60,
                                 env=dict(os.environ, CCURATE_USAGE_INTERRUPT=mode))
            self.assertEqual(run.returncode, 0, run.stderr)
            self.assertIn('Interrupted analysis resumed: ' + mode, run.stderr)
            self.assertEqual(json.loads(run.stdout)['bytes'], 5000431)
            with closing(sqlite3.connect(self.cache)) as cache:
                self.assertEqual(cache.execute('PRAGMA integrity_check').fetchone()[0], 'ok')

    def test_other_segment_balances_loose_files_and_hidden_entries(self):
        for n in range(37):
            self.write(f'.file-{n}', str(n).encode() * (n+1))
        self.app('scan'); data = self.check()
        self.assertTrue(any(n['other'] for n in data['chart']))
        self.assertEqual(data['children'], 43)

    def test_random_directory_unions_against_independent_oracle(self):
        randomizer = random.Random(8273)
        paths = ['', 'a', 'a/x', 'a-', 'a-/deep', 'a0', 'b', 'b/c', 'b/c/d', 'space name', '日本語']
        # Keep these names separate from the baseline A/B folders on Windows.
        paths = ['random/' + path if path else '' for path in paths]
        contents = [hashlib.sha512(str(i).encode()).digest() * (i+1) for i in range(8)]
        for i in range(150):
            parent = randomizer.choice(paths)
            self.write((parent + '/' if parent else '') + f'item-{i}', randomizer.choice(contents))
        self.app('scan')
        self.check()
        self.sql("UPDATE entries SET discarded=1 WHERE id%7=0")
        for scope in [0, 1]:
            for path in paths:
                self.check(path, scope)

    @unittest.skipIf(os.name == 'nt', 'Unix symlink fixture')
    def test_refuses_symlinked_analysis_cache(self):
        protected = self.base / 'protected'
        protected.write_text('do not change')
        self.cache.symlink_to(protected)
        result = subprocess.run([DRIVER, str(self.destination)], capture_output=True, text=True, timeout=30)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('Unsafe analysis cache file', result.stderr)
        self.assertEqual(protected.read_text(), 'do not change')


if __name__ == '__main__':
    unittest.main(verbosity=2)
