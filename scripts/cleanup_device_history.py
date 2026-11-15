#!/usr/bin/env python3
"""Remove redundant device-only observations from an offline CCurate catalog.

Default: dry run. --apply takes a SQLite backup before making changes. The source
and curated data files are never opened. Python 3.9+, standard library only.
"""
import argparse
import base64
from contextlib import closing, contextmanager
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import shutil
import sqlite3
import stat
import sys
import time


@contextmanager
def session_lock(path):
    fd = os.open(path, os.O_RDWR | getattr(os, 'O_NOFOLLOW', 0))
    try:
        if not stat.S_ISREG(os.fstat(fd).st_mode):
            raise RuntimeError('Unsafe session.lock')
        if os.name == 'nt':
            import ctypes
            import msvcrt
            from ctypes import wintypes
            class Overlapped(ctypes.Structure):
                _fields_ = [('Internal', ctypes.c_size_t), ('InternalHigh', ctypes.c_size_t),
                            ('Offset', wintypes.DWORD), ('OffsetHigh', wintypes.DWORD),
                            ('hEvent', wintypes.HANDLE)]
            kernel = ctypes.WinDLL('kernel32', use_last_error=True)
            kernel.LockFileEx.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.DWORD,
                                         wintypes.DWORD, wintypes.DWORD, ctypes.POINTER(Overlapped)]
            kernel.LockFileEx.restype = wintypes.BOOL
            overlap = Overlapped()
            if not kernel.LockFileEx(msvcrt.get_osfhandle(fd), 3, 0, 0xffffffff, 0xffffffff,
                                     ctypes.byref(overlap)):
                raise RuntimeError('Destination is in use. Close CCurate before cleanup.')
        else:
            import fcntl
            try:
                fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError as error:
                raise RuntimeError('Destination is in use. Close CCurate before cleanup.') from error
        yield
    finally:
        os.close(fd)


def safe_files(database):
    # Refuse symlinks and special files before SQLite opens its sidecars.
    for parent in (database.parent, database.parent.parent):
        if parent.is_symlink():
            raise RuntimeError(f'Symlinked session directory: {parent}')
    for name in (database.name, database.name + '-wal', database.name + '-shm',
                 database.name + '-journal', 'session.lock', 'archive.jsonl'):
        path = database.parent / name
        try:
            mode = path.lstat().st_mode
        except FileNotFoundError:
            if name in (database.name, 'session.lock'):
                raise RuntimeError(f'Missing session file: {path}')
            continue
        if not stat.S_ISREG(mode):
            raise RuntimeError(f'Unsafe session file: {path}')


def connect(path, writable=False):
    return sqlite3.connect(path.as_uri() + ('?mode=rw' if writable else '?mode=ro'),
                           uri=True, timeout=2, isolation_level=None)


def validate(db):
    version = db.execute('PRAGMA user_version').fetchone()[0]
    if version not in (2, 3):
        raise RuntimeError(f'Unsupported catalog schema {version}; expected 2 or 3')
    if db.execute("SELECT 1 FROM operations WHERE state='pending' LIMIT 1").fetchone():
        raise RuntimeError('Pending file operations exist. Open CCurate to recover them first.')
    expected = {'id', 'eid', 'mode', 'size', 'uid', 'gid', 'mtime', 'ctime', 'atime', 'btime',
                'dev', 'ino', 'hash', 'link', 'attrs', 'meta_error', 'accepted'}
    if version == 3:
        expected.add('filesystem')
    columns = [r[1] for r in db.execute('PRAGMA table_info(versions)')]
    if set(columns) != expected:
        raise RuntimeError('Unexpected versions schema; refusing to discard unknown metadata')
    allowed_refs = {('empty_copies', 'version'), ('rejected_retention', 'version')}
    for (table,) in db.execute("SELECT name FROM sqlite_master WHERE type='table'").fetchall():
        quoted = '"' + table.replace('"', '""') + '"'
        for row in db.execute(f'PRAGMA foreign_key_list({quoted})'):
            if row[2] == 'versions' and (table, row[3]) not in allowed_refs:
                raise RuntimeError(f'Unknown reference to versions: {table}.{row[3]}')
    check_integrity(db)
    return version, columns


def check_integrity(db):
    error = db.execute('PRAGMA foreign_key_check').fetchone()
    if error:
        raise RuntimeError(f'Foreign-key check failed: {error}')
    bad = db.execute('SELECT e.id FROM entries e LEFT JOIN versions v ON v.id=e.current '
                     'WHERE e.current IS NOT NULL AND (v.id IS NULL OR v.eid<>e.id) LIMIT 1').fetchone()
    if bad:
        raise RuntimeError(f'Invalid current-version reference at entry {bad[0]}')


def plan(db, columns):
    db.execute('PRAGMA temp_store=FILE')
    db.execute('CREATE TEMP TABLE cleanup_map(old INTEGER PRIMARY KEY, keep INTEGER NOT NULL)')
    # The existing (eid) index orders each entry's versions by rowid/id, avoiding
    # a whole-catalog RAM sort. Only one record and a small mapping batch live in Python.
    compared = [c for c in columns if c not in ('id', 'dev')]
    select = ','.join('v.' + c for c in compared)
    factory = db.text_factory
    db.text_factory = bytes
    rows = db.execute('SELECT v.id,v.dev,' + select + ', '
                      '(SELECT 1 FROM rejected_retention r WHERE r.version=v.id),'
                      'z.dest,z.state,z.verified FROM versions v INDEXED BY version_entry '
                      'LEFT JOIN empty_copies z ON z.version=v.id ORDER BY v.eid,v.id')
    previous, keep, pending = None, None, []
    total = redundant = legacy = 0
    for row in rows:
        total += 1
        legacy += row[1] is not None
        signature = row[2:]
        if signature == previous:
            redundant += 1
            pending.append((row[0], keep))
        else:
            previous, keep = signature, row[0]
        if len(pending) >= 4096:
            db.executemany('INSERT INTO cleanup_map VALUES(?,?)', pending)
            pending.clear()
        if total % 100000 == 0:
            print(f'Inspected {total:,} versions; {redundant:,} redundant', flush=True)
    db.executemany('INSERT INTO cleanup_map VALUES(?,?)', pending)
    db.text_factory = factory
    discoveries = db.execute('SELECT COUNT(*) FROM discoveries WHERE dev IS NOT NULL').fetchone()[0]
    return dict(versions=total, redundant=redundant, legacy=legacy, discoveries=discoveries)


def backup_catalog(database, backup):
    if backup.parent.resolve() == database.parent.resolve() and backup.name in (
            database.name, database.name + '-wal', database.name + '-shm', 'session.lock', 'archive.jsonl'):
        raise RuntimeError('Backup must have a separate filename')
    # CREATE_EXCLUSIVE prevents replacing any existing backup or symlink.
    fd = os.open(backup, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    os.close(fd)
    # A second read connection sees the last committed state while the writer
    # holds BEGIN IMMEDIATE. Calling backup() on the writer would deadlock.
    with closing(connect(database)) as src, closing(sqlite3.connect(backup)) as dst:
        src.backup(dst, pages=4096)
        if dst.execute('PRAGMA quick_check').fetchone() != ('ok',):
            raise RuntimeError('Backup integrity check failed')
    with backup.open('r+b') as f:
        os.fsync(f.fileno())
    manifest = database.parent / 'archive.jsonl'
    if manifest.exists():
        copy = Path(str(backup) + '.archive.jsonl')
        fd = os.open(copy, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(fd, 'wb') as out, manifest.open('rb') as inp:
            shutil.copyfileobj(inp, out)
            out.flush(); os.fsync(out.fileno())
    print(f'Backup: {backup}', flush=True)


def path_fields(key, value):
    raw = value or b''
    if isinstance(raw, str):
        raw = raw.encode('utf-8')
    return {key: raw.decode('utf-8', errors='replace'), key + '_b64': base64.b64encode(raw).decode('ascii')}


def text(value):
    return value.decode('utf-8', errors='replace') if isinstance(value, bytes) else value


def write_manifest(db, filename, schema):
    # Match the retained-only portable index; paths remain authoritative raw bytes.
    factory = db.text_factory
    db.text_factory = bytes
    try:
        fd = os.open(filename, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(fd, 'w', encoding='utf-8', newline='\n') as f:
            def emit(record):
                f.write(json.dumps(record, ensure_ascii=True, separators=(',', ':')) + '\n')
            source = db.execute("SELECT value FROM settings WHERE key='source'").fetchone()
            emit(dict(type='header', schema=schema, hash_algorithm='sha512', generated_ns=str(time.time_ns()),
                      encoding='path_b64 is authoritative POSIX bytes',
                      **path_fields('source', source[0] if source else b'')))
            if schema == 3:
                for row in db.execute('SELECT id,identity,name FROM source_filesystems ORDER BY id'):
                    emit(dict(type='filesystem', id=row[0], identity=text(row[1]), name=text(row[2])))
            for row in db.execute('SELECT hash,size,dest,state,verified FROM objects WHERE dest IS NOT NULL '
                                  'AND hash IN(SELECT hash FROM versions WHERE accepted=1)'):
                emit(dict(type='object', sha512=text(row[0]), size=row[1], state=text(row[3]),
                          verified_ns=str(row[4]), **path_fields('destination', row[2])))
            fs = 'v.filesystem' if schema == 3 else 'NULL'
            rows = db.execute('SELECT e.path,e.kind,v.id,v.mode,v.size,v.uid,v.gid,v.mtime,v.ctime,'
                              'v.atime,v.btime,' + fs + ',v.ino,v.hash,v.link,v.attrs,v.meta_error,'
                              'v.id=e.current,z.dest,z.state FROM versions v JOIN entries e ON e.id=v.eid '
                              'LEFT JOIN empty_copies z ON z.version=v.id WHERE v.accepted=1 ORDER BY e.path,v.id')
            kinds = {0o100000: 'file', 0o040000: 'directory', 0o120000: 'symlink', 0o010000: 'fifo',
                     0o140000: 'socket', 0o060000: 'block', 0o020000: 'character'}
            for r in rows:
                record = dict(type='entry', kind=kinds.get((r[3] or 0) & 0o170000, text(r[1])), version=r[2],
                              mode=r[3], size=r[4], uid=r[5], gid=r[6], mtime_ns=str(r[7]), ctime_ns=str(r[8]),
                              atime_ns=str(r[9]), btime_ns=str(r[10]), ino=str(r[12]),
                              xattrs=json.loads(r[15]), metadata_error=text(r[16]), current_at_export=bool(r[17]),
                              **path_fields('path', r[0]))
                if schema == 3:
                    record['filesystem'] = r[11]
                if r[13] is not None:
                    record['sha512'] = text(r[13])
                if r[14] is not None:
                    record.update(path_fields('link', r[14]))
                if r[18] is not None:
                    record.update(path_fields('destination', r[18])); record['destination_state'] = text(r[19])
                emit(record)
            for r in db.execute("SELECT id,time,op,src,dst,hash FROM events WHERE op IN "
                                "('copy','reuse','move','recover','remove') ORDER BY id"):
                record = dict(type='event', id=r[0], time_ns=str(r[1]), operation=text(r[2]),
                              **path_fields('from', r[3]), **path_fields('to', r[4]))
                if r[5] is not None:
                    record['sha512'] = text(r[5])
                emit(record)
            f.flush(); os.fsync(f.fileno())
    finally:
        db.text_factory = factory


def apply_plan(db):
    # Remap current while both records exist, so duplicate/usage triggers still
    # see identical content. Never touch review/reject decisions or source paths.
    db.execute('UPDATE entries SET current=(SELECT keep FROM cleanup_map WHERE old=current) '
               'WHERE current IN(SELECT old FROM cleanup_map)')
    for table, column in [('empty_copies', 'version'), ('rejected_retention', 'version')]:
        # plan() only merges records with exactly equal retention/copy states;
        # the keeper already has the equivalent row (including verification time).
        db.execute(f'DELETE FROM {table} WHERE {column} IN(SELECT old FROM cleanup_map)')
    db.execute('DELETE FROM versions WHERE id IN(SELECT old FROM cleanup_map)')
    db.execute('UPDATE versions SET dev=NULL WHERE dev IS NOT NULL')
    db.execute('UPDATE discoveries SET dev=NULL WHERE dev IS NOT NULL')
    check_integrity(db)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('destination', type=Path, help='Destination root containing .ccurate')
    parser.add_argument('--apply', action='store_true', help='Back up and apply the printed cleanup plan')
    parser.add_argument('--backup', type=Path, help='New backup filename (may be on another disk)')
    parser.add_argument('--vacuum', action='store_true', help='Reclaim database file space after cleanup; needs extra disk space')
    args = parser.parse_args()
    if args.vacuum and not args.apply:
        parser.error('--vacuum requires --apply')
    database = args.destination.absolute() / '.ccurate/catalog.sqlite'
    safe_files(database)
    with session_lock(database.parent / 'session.lock'), closing(connect(database, args.apply)) as db:
        db.execute('PRAGMA foreign_keys=ON')
        db.execute('PRAGMA busy_timeout=2000')
        db.execute('PRAGMA cache_size=-16384')
        db.execute('PRAGMA temp_store=FILE')
        db.execute('BEGIN IMMEDIATE' if args.apply else 'BEGIN')
        schema, columns = validate(db)
        stats = plan(db, columns)
        print('Plan: ' + ', '.join(f'{key}={value:,}' for key, value in stats.items()), flush=True)
        print('Only adjacent identical records (except legacy device) with identical retention and copy states are merged.')
        if not args.apply:
            db.rollback()
            print('Dry run: catalog and JSONL unchanged. Use --apply to back up and clean.')
            return
        stamp = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S.%fZ')
        backup = (args.backup or database.with_name(f'catalog.before-device-cleanup.{stamp}.sqlite')).absolute()
        estimated = db.execute('PRAGMA page_count').fetchone()[0] * db.execute('PRAGMA page_size').fetchone()[0]
        manifest = database.parent / 'archive.jsonl'
        manifest_size = manifest.stat().st_size if manifest.exists() else 0
        if shutil.disk_usage(backup.parent).free < estimated + manifest_size:
            raise RuntimeError('Insufficient space for backup. Use --backup on a disk with more free space.')
        # The transaction WAL may approach the current catalog size. VACUUM can
        # need roughly twice that size in addition to the backup.
        reserve = estimated * (3 if args.vacuum else 1) + manifest_size
        if backup.parent.stat().st_dev == database.parent.stat().st_dev:
            reserve += estimated + manifest_size
        if shutil.disk_usage(database.parent).free < reserve:
            raise RuntimeError(f'Cleanup needs approximately {reserve:,} free bytes on the destination for backup/WAL/temp files.')
        backup_catalog(database, backup)
        temporary = database.parent / f'archive.device-cleanup.{stamp}.tmp'
        committed = False
        try:
            apply_plan(db)
            write_manifest(db, temporary, schema)
            db.commit()
            committed = True
            os.replace(temporary, manifest)
            if os.name != 'nt':
                fd = os.open(database.parent, os.O_RDONLY | os.O_DIRECTORY)
                try:
                    os.fsync(fd)
                finally:
                    os.close(fd)
        except BaseException:
            if not committed:
                db.rollback()
            else:
                print('Catalog committed; JSONL publication failed. Keep the backup and rerun this tool to regenerate JSONL.', file=sys.stderr)
            raise
        finally:
            temporary.unlink(missing_ok=True)
        db.execute('PRAGMA wal_checkpoint(TRUNCATE)')
        if args.vacuum:
            print('Vacuuming the catalog; this can take a long time...', flush=True)
            db.execute('VACUUM')
            db.execute('PRAGMA wal_checkpoint(TRUNCATE)')
        print(f'Cleaned {stats["redundant"]:,} redundant versions; legacy device fields cleared; JSONL regenerated.')
        print('Existing decisions preserved. Previously lost decisions cannot be reconstructed from device history.')
        if not args.vacuum:
            print('Freed SQLite pages are reusable; use --vacuum to reduce the database file size.')


if __name__ == '__main__':
    try:
        main()
    except (OSError, sqlite3.Error, RuntimeError, ValueError) as error:
        print(f'Cleanup failed: {error}', file=sys.stderr)
        sys.exit(1)
