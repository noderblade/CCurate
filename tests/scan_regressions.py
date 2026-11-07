"""Shared Linux/Windows regressions for empty files and deferred source reads."""
from contextlib import closing
import base64
import hashlib
import json
import os
import shutil
import sqlite3

EMPTY = hashlib.sha512(b'').hexdigest()


class ScanRegressions:
    def empty_fixture(self):
        for path in ('A/marker', 'B/marker', 'B/other'):
            (self.src / path).write_bytes(b'')
        self.run_app('scan')

    def empty_rows(self):
        return self.sql("SELECT e.path,v.accepted,z.dest,z.state FROM entries e "
                        "JOIN versions v ON v.id=e.current LEFT JOIN empty_copies z ON z.version=v.id "
                        "WHERE e.kind='file' AND v.size=0 ORDER BY e.path")

    def test_empty_files_are_independent(self):
        self.empty_fixture()
        self.assertEqual(self.sql('SELECT COUNT(*) FROM objects WHERE hash=?', (EMPTY,)), [(0,)])
        self.assertEqual(self.sql('SELECT COUNT(*) FROM hash_groups WHERE hash=?', (EMPTY,)), [(0,)])
        info = self.run_app('info', 'A/marker').stdout
        self.assertIn(b'EMPTY FILE', info)
        self.assertNotIn(b'CONTENT OCCURRENCES', info)
        self.run_app('copy', 'A/marker', 'First')
        self.assertEqual(self.empty_rows(), [('A/marker', 1, 'First/marker', 'verified'),
                                           ('B/marker', 0, None, None), ('B/other', 0, None, None)])
        self.assertIn(b'B/marker', self.run_app('list', 'B', '--unsecured').stdout)
        self.run_app('copy', 'B/marker', 'Second')
        self.run_app('copy', 'B/other', 'Second')
        for path in ('First/marker', 'Second/marker', 'Second/other'):
            self.assertEqual((self.dst / path).read_bytes(), b'')
        self.assertEqual(len({(self.dst / p).stat().st_ino for p in ('First/marker', 'Second/marker')}), 2)
        self.run_app('reject', 'A/marker')
        self.assertNotIn('A/marker', {r.get('path') for r in self.manifest()})
        self.run_app('restore', 'A/marker')
        self.assertIn('A/marker', {r.get('path') for r in self.manifest()})

    def test_empty_collision_and_new_metadata_version(self):
        self.empty_fixture()
        self.run_app('copy', 'A/marker', 'First')
        self.run_app('copy', 'B/marker', 'First', ok=False)
        self.assertEqual(self.empty_rows()[1][1:], (0, None, None))
        original = (self.src / 'A/marker').stat()
        os.utime(self.src / 'A/marker', ns=(original.st_atime_ns, original.st_mtime_ns + 2_000_000_000))
        self.run_app('scan')
        self.assertEqual(self.empty_rows()[0][1:], (0, None, None))
        self.run_app('copy', 'A/marker', 'First')
        records = [r for r in self.manifest() if r.get('path') == 'A/marker']
        self.assertEqual(len(records), 2)
        self.assertEqual({r['destination'] for r in records}, {'First/marker'})
        self.assertEqual(len({r['mtime_ns'] for r in records}), 2)
        self.run_app('audit')

    def test_empty_move_audit_and_offline_import(self):
        self.empty_fixture()
        self.run_app('copy', 'A', 'First', '--complete')
        self.run_app('copy', 'B/marker', 'Second')
        self.run_app('move', 'First', 'Relocated')
        self.assertEqual(self.empty_rows()[0][2], 'Relocated/A/marker')
        (self.dst / 'Second/marker').write_bytes(b'corrupt')
        self.run_app('audit', ok=False)
        self.assertEqual([r[3] for r in self.empty_rows()], ['verified', 'corrupt', None])
        (self.dst / 'Second/marker').write_bytes(b'')
        self.run_app('audit')
        records = self.manifest()
        self.assertEqual(records[0]['schema'], 2)
        self.assertFalse(any(r.get('type') == 'object' and r.get('size') == 0 for r in records))
        fresh = self.root / 'rebuilt'; fresh.mkdir()
        shutil.copytree(self.dst / 'Relocated', fresh / 'Relocated')
        shutil.copytree(self.dst / 'Second', fresh / 'Second')
        self.run_app('import', str(self.dst / '.ccurate/archive.jsonl'), source='-', dest=fresh)
        self.run_app('audit', source='-', dest=fresh)
        listing = self.run_app('list', 'A', '--history', source='-', dest=fresh).stdout
        self.assertIn(b'verified\tA/marker\tRelocated/A/marker', listing)
        self.assertNotIn(b'B/other', self.run_app('list', 'B', '--history', source='-', dest=fresh).stdout)

    def test_empty_recovery_after_publish(self):
        self.empty_fixture()
        (self.dst / 'recovered').write_bytes(b'')
        self.sql("INSERT INTO operations(kind,src,dst,temp,hash) VALUES('copy','A/marker','recovered','missing.part',?)", (EMPTY,))
        self.run_app('export')
        self.assertEqual(self.empty_rows()[0][1:], (1, 'recovered', 'verified'))
        self.assertEqual(self.empty_rows()[1][1:], (0, None, None))

    def test_schema_one_empty_mapping_migration(self):
        self.empty_fixture()
        (self.dst / 'legacy-empty').write_bytes(b'')
        with closing(sqlite3.connect(self.dst / '.ccurate/catalog.sqlite')) as db, db:
            db.execute("INSERT INTO objects(hash,size,dest,state) VALUES(?,0,'legacy-empty','verified')", (EMPTY,))
            db.execute("UPDATE versions SET accepted=1 WHERE hash=? AND eid IN(SELECT id FROM entries WHERE path IN('A/marker','B/marker'))", (EMPTY,))
            db.execute("INSERT INTO rejected_retention SELECT current FROM entries WHERE path='B/marker'")
            db.execute("UPDATE versions SET accepted=0 WHERE id=(SELECT current FROM entries WHERE path='B/marker')")
            db.execute("UPDATE entries SET discarded=1 WHERE path='B/marker'")
            db.execute('DROP TABLE empty_copies')
            db.execute('PRAGMA user_version=1')
        self.run_app('export')
        self.assertEqual(self.sql('PRAGMA user_version'), [(2,)])
        self.assertEqual(self.empty_rows(), [('A/marker', 1, 'legacy-empty', 'verified'),
                                           ('B/marker', 0, 'legacy-empty', 'verified'), ('B/other', 0, None, None)])
        self.run_app('restore', 'B/marker')
        self.assertEqual(self.empty_rows()[1][1], 1)
        self.run_app('copy', 'B/other', 'New')
        self.assertEqual((self.dst / 'New/other').read_bytes(), b'')
        self.assertEqual((self.dst / 'legacy-empty').read_bytes(), b'')
        self.assertEqual(self.sql('SELECT COUNT(*) FROM hash_groups WHERE hash=?', (EMPTY,)), [(0,)])
        self.run_app('audit')

    def test_schema_one_empty_manifest_import(self):
        self.empty_fixture()
        self.run_app('copy', 'A/marker', 'First')
        records = self.manifest(); records[0]['schema'] = 1
        for record in records:
            if record.get('type') == 'entry':
                for key in ('destination', 'destination_b64', 'destination_state'):
                    record.pop(key, None)
        records.insert(1, dict(type='object', size=0, sha512=EMPTY, destination='legacy-empty',
                              destination_b64=base64.b64encode(b'legacy-empty').decode(), state='verified'))
        manifest = self.root / 'schema1.jsonl'
        manifest.write_text('\n'.join(json.dumps(r) for r in records) + '\n', encoding='utf-8')
        fresh = self.root / 'legacy-import'; fresh.mkdir()
        (fresh / 'legacy-empty').write_bytes(b'')
        self.run_app('import', str(manifest), source='-', dest=fresh)
        self.run_app('audit', source='-', dest=fresh)
        self.assertIn(b'verified\tA/marker\tlegacy-empty', self.run_app('list', 'A', '--history', source='-', dest=fresh).stdout)

    def test_open_writer_deferred_without_losing_history(self):
        self.run_app('copy', 'A/one.txt', 'Curated')
        self.run_app('review', 'A/one.txt')
        versions = self.sql("SELECT COUNT(*) FROM versions WHERE eid=(SELECT id FROM entries WHERE path='A/one.txt')")
        # Writable but idle handles must also be deferred; read-only handles must not.
        with (self.src / 'A/one.txt').open('r+b'):
            self.run_app('scan', ok=False)
            self.assertTrue(self.sql("SELECT error FROM entries WHERE path='A/one.txt'")[0][0].startswith('UNSCANNED:'))
            self.assertIn(b'UNSCANNED', self.run_app('list', 'A', '--unsecured').stdout)
            self.assertIn(b'Curated/one.txt', self.run_app('list', 'A', '--history').stdout)
            self.run_app('copy', 'A/one.txt', 'Unsafe', ok=False)
            self.assertFalse((self.dst / 'Unsafe/one.txt').exists())
        self.run_app('scan')
        self.assertEqual(self.sql("SELECT error,reviewed FROM entries WHERE path='A/one.txt'"), [('', 1)])
        self.assertEqual(self.sql("SELECT COUNT(*) FROM versions WHERE eid=(SELECT id FROM entries WHERE path='A/one.txt')"), versions)
        with (self.src / 'A/one.txt').open('rb'):
            self.run_app('scan', '--full')
        self.assertIn('A/one.txt', {r.get('path') for r in self.manifest()})

    def test_new_writable_file_does_not_stop_other_files(self):
        (self.src / 'zz-stable').write_bytes(b'read this even if another file is busy')
        with (self.src / 'aa-active').open('w+b', buffering=0) as writer:
            writer.write(b'being edited')
            self.run_app('scan', ok=False)
            self.assertIn(b'UNSCANNED', self.run_app('info', 'aa-active').stdout)
            self.assertEqual(self.sql("SELECT current FROM entries WHERE path='aa-active'"), [(None,)])
            self.run_app('copy', 'zz-stable', 'Kept')
            writer.write(b' finished')
        self.run_app('scan')
        self.run_app('copy', 'aa-active', 'Kept')
        self.assertEqual((self.dst / 'Kept/aa-active').read_bytes(), b'being edited finished')
        log = (self.dst / '.ccurate/activity.log').read_text(encoding='utf-8')
        self.assertTrue(any('WARN' in line and 'UNSCANNED' in line for line in log.splitlines()))
