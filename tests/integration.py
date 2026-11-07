#!/usr/bin/env python3
"""Black-box tests against real files; all fixtures are disposable directories."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import sqlite3
import subprocess
import sys
import tempfile
import unittest
from scan_regressions import ScanRegressions

APP = str(Path(sys.argv.pop(1)).resolve())


class Curation(ScanRegressions, unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="ccurate-test-")
        self.root = Path(self.temp.name)
        self.src, self.dst = self.root / "old", self.root / "new"
        self.src.mkdir(); self.dst.mkdir()
        (self.src / "A/empty").mkdir(parents=True)
        (self.src / "B").mkdir()
        (self.src / "A/one.txt").write_bytes(b"precious\x00data\n")
        (self.src / "B/copy.bin").write_bytes(b"precious\x00data\n")
        (self.src / ".hidden").write_text("also important")
        (self.src / "junk.tmp").write_text("discard this")
        (self.src / "link").symlink_to("A/one.txt")
        os.mkfifo(self.src / "named-pipe")
        os.chmod(self.src / "A/one.txt", 0o640)
        try:
            os.setxattr(self.src / "A/one.txt", "user.test", b"exact\x00attribute")
        except OSError:
            pass
        self.run_app("scan")

    def tearDown(self):
        self.temp.cleanup()

    def run_app(self, *args, ok=True, source=None, dest=None):
        r = subprocess.run([APP, "--cli", str(source or self.src), str(dest or self.dst), *args],
                           capture_output=True, timeout=30)
        if ok:
            self.assertEqual(r.returncode, 0, r.stderr.decode(errors="replace"))
        else:
            self.assertNotEqual(r.returncode, 0, r.stdout.decode(errors="replace"))
        return r

    def sql(self, q, params=()):
        with sqlite3.connect(self.dst / ".ccurate/catalog.sqlite") as db:
            return db.execute(q, params).fetchall()

    def manifest(self):
        return [json.loads(line) for line in (self.dst / ".ccurate/archive.jsonl").read_text().splitlines()]

    @unittest.skipIf(os.geteuid() == 0, "permission test needs an unprivileged process")
    def test_unreadable_directory_preserves_children_and_error(self):
        os.chmod(self.src / "A", 0)
        try:
            self.run_app("scan", ok=False)
            self.assertEqual(self.sql("SELECT present FROM entries WHERE path='A/one.txt'"), [(1,)])
            self.assertTrue(self.sql("SELECT error FROM entries WHERE path='A'")[0][0])
            self.assertEqual(self.sql("SELECT value FROM settings WHERE key='scan_complete'"), [("0",)])
        finally:
            os.chmod(self.src / "A", 0o755)
        self.run_app("scan")
        self.assertEqual(self.sql("SELECT error FROM entries WHERE path='A'"), [("",)])

    @unittest.skipIf(os.geteuid() == 0, "permission test needs an unprivileged process")
    def test_activity_errors_append_with_paths_and_without_source_changes(self):
        log = self.dst / ".ccurate/activity.log"
        before = log.read_bytes()
        file = self.src / "bad\nname.txt"
        file.write_bytes(b"untouched content")
        file.chmod(0)
        try:
            self.run_app("scan", ok=False)
        finally:
            file.chmod(0o600)
        after = log.read_bytes()
        self.assertTrue(after.startswith(before))
        records = after.decode().splitlines()
        errors = [x for x in records if " | ERROR | " in x]
        self.assertTrue(any(r'bad\\x0aname.txt' in x for x in errors), errors)
        self.assertTrue(any("Permission denied" in x for x in errors), errors)
        self.assertEqual(file.read_bytes(), b"untouched content")
        self.run_app("copy", "A/one.txt", "Curated")
        self.run_app("reject", "junk.tmp")
        content = log.read_text()
        self.assertIn("session.open", content)
        self.assertIn("scan.finish", content)
        self.assertIn("copy", content)
        self.assertIn("reject", content)
        self.assertEqual(log.stat().st_mode & 0o777, 0o600)

    def test_activity_refuses_symlink(self):
        log = self.dst / ".ccurate/activity.log"
        log.unlink()
        victim = self.root / "must-stay-unchanged.txt"
        victim.write_text("untouched")
        log.symlink_to(victim)
        self.run_app("scan", ok=False)
        self.assertEqual(victim.read_text(), "untouched")

    def test_activity_refuses_hardlink_and_fifo(self):
        log = self.dst / ".ccurate/activity.log"
        log.unlink()
        victim = self.src / "A/one.txt"
        before = victim.read_bytes()
        os.link(victim, log)
        self.run_app("scan", ok=False)
        self.assertEqual(victim.read_bytes(), before)
        log.unlink()
        os.mkfifo(log)
        self.run_app("scan", ok=False)

    def test_duplicate_cache_tracks_changes_and_reappearance(self):
        digest = hashlib.sha512((self.src / "A/one.txt").read_bytes()).hexdigest()
        def copies():
            return self.sql("SELECT occurrences FROM hash_groups WHERE hash=?", (digest,))[0][0]
        self.assertEqual(copies(), 2)
        (self.src / "B/copy.bin").unlink()
        self.run_app("scan")
        self.assertEqual(copies(), 1)
        (self.src / "B/copy.bin").write_bytes((self.src / "A/one.txt").read_bytes())
        self.run_app("scan")
        self.assertEqual(copies(), 2)
        (self.src / "A/one.txt").write_text("new content")
        self.run_app("scan")
        self.assertEqual(copies(), 1)

    def test_inventory_is_not_archive(self):
        paths = {x[0] for x in self.sql("SELECT path FROM entries")}
        self.assertTrue({".hidden", "named-pipe", "link", "A/empty"} <= paths)
        self.assertFalse(any(x["type"] == "entry" for x in self.manifest()))
        count = self.sql("SELECT COUNT(*) FROM versions")[0][0]
        self.run_app("scan")
        self.assertEqual(count, self.sql("SELECT COUNT(*) FROM versions")[0][0])

    def test_destination_cannot_contain_source(self):
        result = self.run_app("scan", dest=self.root, ok=False)
        self.assertIn(b"must not contain the source", result.stderr)
        self.assertEqual((self.src / "A/one.txt").read_bytes(), b"precious\x00data\n")

    def test_unsecured_filter_includes_unhandled_empty_directory(self):
        self.run_app("copy", "A/one.txt", "Curated")
        remaining = self.run_app("list", "A", "--unsecured").stdout.decode()
        self.assertIn("A/empty", remaining)
        self.assertNotIn("A/one.txt", remaining)
        self.assertIn("\tA\t", self.run_app("list", "", "--unsecured").stdout.decode())
        self.run_app("copy", "A", "Curated", "--complete")
        self.assertNotIn("\tA\t", self.run_app("list", "", "--unsecured").stdout.decode())
        self.assertEqual(self.run_app("list", "A", "--unsecured").stdout, b"")
        self.run_app("copy", "link", "Curated")
        self.assertNotIn("\tlink\t", self.run_app("list", "", "--unsecured").stdout.decode())

    def test_metadata_rejection_can_be_undone(self):
        self.run_app("copy", "A/empty", "Curated", "--complete")
        self.run_app("copy", "link", "Curated")
        self.run_app("reject", "A/empty")
        self.run_app("reject", "link")
        paths = {x.get("path") for x in self.manifest()}
        self.assertNotIn("A/empty", paths)
        self.assertNotIn("link", paths)
        self.run_app("restore", "A/empty")
        self.run_app("restore", "link")
        paths = {x.get("path") for x in self.manifest()}
        self.assertTrue({"A/empty", "link"} <= paths)

    def test_dedup_and_provenance(self):
        self.run_app("copy", "B/copy.bin", "Curated")
        self.assertEqual((self.dst / "Curated/copy.bin").read_bytes(), (self.src / "A/one.txt").read_bytes())
        self.run_app("copy", "A/one.txt", "Elsewhere")
        self.assertFalse((self.dst / "Elsewhere/one.txt").exists())
        entries = {x["path"]: x for x in self.manifest() if x["type"] == "entry"}
        self.assertTrue({"", "A", "B", "A/one.txt", "B/copy.bin"} <= entries.keys())
        self.assertEqual(entries["A"]["kind"], "directory")
        self.assertEqual(entries["B"]["kind"], "directory")
        self.assertEqual(entries["A/one.txt"]["mode"] & 0o777, 0o640)
        self.assertNotIn("junk.tmp", entries)
        self.assertEqual(len([x for x in self.manifest() if x["type"] == "object"]), 1)

    def test_reject_is_reversible_and_per_path(self):
        self.run_app("reject", "B")
        self.run_app("copy", "A/one.txt", "")
        self.assertNotIn("B/copy.bin", {x.get("path") for x in self.manifest()})
        self.assertTrue((self.src / "B/copy.bin").exists())
        self.run_app("restore", "B")
        self.assertIn("B/copy.bin", {x.get("path") for x in self.manifest()})

    def test_live_versions_and_deletions(self):
        self.run_app("copy", "A/one.txt", "v1")
        self.run_app("review", "A")
        (self.src / "A/one.txt").write_bytes(b"updated version")
        (self.src / "new-file").write_bytes(b"new")
        (self.src / ".hidden").unlink()
        self.run_app("scan")
        row = self.sql("SELECT e.reviewed,v.accepted,o.state FROM entries e JOIN versions v ON v.id=e.current JOIN objects o ON o.hash=v.hash WHERE e.path='A/one.txt'")[0]
        self.assertEqual(row, (0, 0, "missing"))
        self.assertEqual(self.sql("SELECT present FROM entries WHERE path='.hidden'")[0][0], 0)
        self.run_app("copy", "A/one.txt", "v2")
        versions = [x for x in self.manifest() if x.get("path") == "A/one.txt"]
        self.assertEqual(len(versions), 2)
        self.assertEqual((self.dst / "v1/one.txt").read_bytes(), b"precious\x00data\n")

    def test_changed_source_is_not_copied_using_stale_hash(self):
        (self.src / "A/one.txt").write_text("changed without scan")
        self.run_app("copy", "A/one.txt", "", ok=False)
        self.assertFalse((self.dst / "one.txt").exists())

    def test_changed_duplicate_does_not_reuse_old_hash(self):
        self.run_app("copy", "A/one.txt", "")
        (self.src / "B/copy.bin").write_bytes(b"NEW AND IMPORTANT CONTENT")
        self.run_app("copy", "B/copy.bin", "Another", ok=False)
        self.run_app("scan")
        self.run_app("copy", "B/copy.bin", "Another")
        self.assertEqual((self.dst / "Another/copy.bin").read_bytes(), b"NEW AND IMPORTANT CONTENT")

    def test_full_rescan_does_not_create_fake_versions(self):
        self.run_app("copy", "A/one.txt", "")
        before = self.sql("SELECT COUNT(*) FROM versions")[0][0]
        self.run_app("scan", "--full")
        self.assertEqual(self.sql("SELECT COUNT(*) FROM versions")[0][0], before)

    def test_sparse_file_content(self):
        with open(self.src / "sparse.img", "wb") as f:
            f.write(b"start"); f.seek(8 * 1024 * 1024); f.write(b"end")
        self.run_app("scan")
        self.run_app("copy", "sparse.img", "")
        self.assertEqual(hashlib.sha512((self.src / "sparse.img").read_bytes()).digest(),
                         hashlib.sha512((self.dst / "sparse.img").read_bytes()).digest())

    def test_historical_type_and_directory_metadata(self):
        self.run_app("copy", "link", "")
        (self.src / "link").unlink()
        (self.src / "link").write_text("now a regular file")
        self.run_app("scan")
        old = [x for x in self.manifest() if x.get("path") == "link"]
        self.assertEqual(old[0]["kind"], "symlink")

    def test_no_overwrite_or_path_escape(self):
        (self.dst / "one.txt").write_text("existing other content")
        self.run_app("copy", "A/one.txt", "", ok=False)
        self.assertEqual((self.dst / "one.txt").read_text(), "existing other content")
        self.run_app("copy", "A/one.txt", "../escape", ok=False)
        outside = self.root / "outside"; outside.mkdir()
        (self.dst / "escape").symlink_to(outside, target_is_directory=True)
        self.run_app("copy", "A/one.txt", "escape", ok=False)
        self.assertFalse((outside / "one.txt").exists())

    def test_move_directory_updates_all_links(self):
        self.run_app("copy", "A", "Curated", "--complete")
        self.run_app("move", "Curated", "Final")
        self.assertTrue((self.dst / "Final/A/one.txt").is_file())
        self.assertTrue((self.dst / "Final/A/empty").is_dir())
        self.assertEqual(self.sql("SELECT dest FROM objects WHERE state='verified'")[0][0], "Final/A/one.txt")
        self.run_app("audit")

    def test_audit_and_relocate(self):
        self.run_app("copy", "A/one.txt", "")
        (self.dst / "one.txt").rename(self.dst / "renamed.bin")
        self.run_app("audit", ok=False)
        self.run_app("reconcile")
        self.assertEqual(self.sql("SELECT dest,state FROM objects WHERE dest IS NOT NULL")[0], ("renamed.bin", "verified"))
        (self.dst / "renamed.bin").write_bytes(b"corrupted")
        self.run_app("audit", ok=False)
        self.assertEqual(self.sql("SELECT state FROM objects WHERE dest IS NOT NULL")[0][0], "corrupt")

    def test_rebuild_from_manifest_without_source(self):
        self.run_app("copy", "A", "", "--complete")
        fresh = self.root / "rebuilt"; fresh.mkdir()
        shutil.copytree(self.dst / "A", fresh / "A")
        self.run_app("import", str(self.dst / ".ccurate/archive.jsonl"), source="-", dest=fresh)
        self.run_app("audit", source="-", dest=fresh)
        r = self.run_app("list", "B", "--history", source="-", dest=fresh)
        self.assertIn(b"B/copy.bin", r.stdout)

    def test_non_utf8_names(self):
        name = b"bad_\xff_name"
        path = os.fsencode(self.src) + b"/" + name
        with open(path, "wb") as f: f.write(b"non-utf8-name-data")
        self.run_app("scan")
        self.run_app("copy", os.fsdecode(name), "")
        import base64
        self.assertTrue(any(base64.b64decode(x.get("path_b64", "")) == name for x in self.manifest()))

    def test_recovery_after_publish_before_database_commit(self):
        data = (self.src / "A/one.txt").read_bytes()
        sha = hashlib.sha512(data).hexdigest()
        (self.dst / "recovered.txt").write_bytes(data)
        self.sql("INSERT INTO operations(kind,src,dst,temp,hash) VALUES('copy','A/one.txt','recovered.txt','.ccurate-missing.part',?)", (sha,))
        self.run_app("export")
        self.assertEqual(self.sql("SELECT dest,state FROM objects WHERE hash=?", (sha,))[0], ("recovered.txt", "verified"))

    def test_metadata_only_special_entries(self):
        self.run_app("copy", "link", "")
        self.run_app("copy", "named-pipe", "")
        entries = {x.get("path"): x for x in self.manifest() if x["type"] == "entry"}
        self.assertEqual(entries["link"]["link"], "A/one.txt")
        self.assertEqual(entries["named-pipe"]["kind"], "fifo")
        self.assertFalse((self.dst / "named-pipe").exists())


if __name__ == "__main__": unittest.main(verbosity=2)
