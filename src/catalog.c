#include "curator.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#ifndef _WIN32
#include <linux/magic.h>
#endif
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/file.h>
#include <sys/statvfs.h>
#include <sys/vfs.h>
#include <sys/xattr.h>
#endif
#include <unistd.h>

static const char *schema =
    "PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL; PRAGMA foreign_keys=ON;"
    "CREATE TABLE IF NOT EXISTS settings(key TEXT PRIMARY KEY,value TEXT NOT NULL);"
    "CREATE TABLE IF NOT EXISTS entries(id INTEGER PRIMARY KEY,path TEXT UNIQUE NOT NULL,parent "
    "TEXT,name TEXT,kind TEXT,current INTEGER,present INTEGER DEFAULT 1,seen INTEGER DEFAULT "
    "0,reviewed INTEGER DEFAULT 0,discarded INTEGER DEFAULT 0,error TEXT DEFAULT '');"
    "CREATE INDEX IF NOT EXISTS entry_parent ON entries(parent);"
    "CREATE INDEX IF NOT EXISTS entry_current ON entries(current);"
    "CREATE INDEX IF NOT EXISTS entry_listing ON entries(parent,(kind='directory') DESC,name "
    "COLLATE BINARY);"
    "CREATE TABLE IF NOT EXISTS versions(id INTEGER PRIMARY KEY,eid INTEGER NOT NULL REFERENCES "
    "entries(id),mode INTEGER,size INTEGER,uid INTEGER,gid INTEGER,mtime INTEGER,ctime "
    "INTEGER,atime INTEGER,btime INTEGER,dev INTEGER,ino INTEGER,hash TEXT,link BLOB,attrs TEXT "
    "DEFAULT '[]',meta_error TEXT DEFAULT '',accepted INTEGER DEFAULT 0);"
    "CREATE INDEX IF NOT EXISTS version_hash ON versions(hash);"
    "CREATE INDEX IF NOT EXISTS version_hash_entry ON versions(hash,eid);"
    "CREATE INDEX IF NOT EXISTS version_entry ON versions(eid);"
    "CREATE INDEX IF NOT EXISTS version_retained ON versions(eid,accepted,id);"
    "CREATE INDEX IF NOT EXISTS version_accepted ON versions(accepted,id);"
    "CREATE TABLE IF NOT EXISTS rejected_retention(version INTEGER PRIMARY KEY REFERENCES "
    "versions(id));"
    "CREATE TABLE IF NOT EXISTS objects(hash TEXT PRIMARY KEY,size INTEGER NOT NULL,dest TEXT "
    "UNIQUE,state TEXT DEFAULT 'missing',verified INTEGER DEFAULT 0,copied_from INTEGER);"
    "CREATE TABLE IF NOT EXISTS operations(id INTEGER PRIMARY KEY,kind TEXT,src TEXT,dst TEXT,temp "
    "TEXT,hash TEXT,state TEXT DEFAULT 'pending');"
    "CREATE TABLE IF NOT EXISTS discoveries(eid INTEGER PRIMARY KEY REFERENCES entries(id),"
    "size INTEGER,mtime INTEGER,btime INTEGER,mode INTEGER,uid INTEGER,gid INTEGER,dev INTEGER,ino "
    "INTEGER);"
    "CREATE TABLE IF NOT EXISTS events(id INTEGER PRIMARY KEY,time INTEGER,op TEXT,src TEXT,dst "
    "TEXT,hash TEXT);";
/* Incremental occurrence counts: even a million identical empty files cost one
 * indexed lookup per displayed row, rather than one million-row count per row. */
static const char *group_schema =
    "CREATE TABLE IF NOT EXISTS hash_groups(hash TEXT PRIMARY KEY,occurrences INTEGER NOT NULL);"
    "CREATE TRIGGER IF NOT EXISTS current_group_update AFTER UPDATE OF current,present ON entries "
    "WHEN OLD.current IS NOT NEW.current OR OLD.present<>NEW.present BEGIN "
    "UPDATE hash_groups SET occurrences=occurrences-1 WHERE OLD.present=1 AND hash=(SELECT hash "
    "FROM versions WHERE id=OLD.current);"
    "INSERT INTO hash_groups(hash,occurrences) SELECT hash,1 FROM versions WHERE id=NEW.current "
    "AND NEW.present=1 AND hash IS NOT NULL ON CONFLICT(hash) DO UPDATE SET "
    "occurrences=occurrences+1;END;";
int prepare_hash_groups(Catalog *c)
{
    char *value = setting(c, "hash_groups_ready", "0");
    int ready = !strcmp(value, "1");
    free(value);
    if (ready)
        return 0;
    if (sql_exec(c, "BEGIN IMMEDIATE"))
        return -1;
    int rc = sql_exec(
        c, "DELETE FROM hash_groups;INSERT INTO hash_groups SELECT v.hash,COUNT(*) FROM entries e "
           "JOIN versions v ON v.id=e.current WHERE e.present=1 AND v.hash IS NOT NULL GROUP BY "
           "v.hash;");
    if (!rc)
        rc = set_setting(c, "hash_groups_ready", "1");
    sql_exec(c, rc ? "ROLLBACK" : "COMMIT");
    return rc;
}

int catalog_open(Catalog *c, const char *source, const char *destination, int browse)
{
    memset(c, 0, sizeof(*c));
    c->sfd = c->dfd = c->mfd = c->lockfd = -1;
    pthread_mutex_init(&c->progress_mutex, NULL);
#ifdef _WIN32
    if (!cc_win_validate_roots(source, destination, c->error, sizeof(c->error)))
        return -1;
#endif
    c->destination = realpath(destination, NULL);
    if (!c->destination)
        return fail(c, "Destination: %s", strerror(errno));
    c->dfd = open(c->destination, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (c->dfd < 0)
        return fail(c, "Destination: %s", strerror(errno));
    if (mkdirat(c->dfd, META_DIR, 0700) < 0 && errno != EEXIST)
        return fail(c, "State directory: %s", strerror(errno));
    c->mfd = openat(c->dfd, META_DIR, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (c->mfd < 0)
        return fail(c, "Unsafe state directory");
    c->lockfd = openat(c->mfd, "session.lock", O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (c->lockfd < 0 || flock(c->lockfd, LOCK_EX | LOCK_NB))
        return fail(c, "This destination already has an active session");
    c->state_dir = join_path(c->destination, META_DIR);
    char *dbpath = join_path(c->state_dir, "catalog.sqlite");
    /* Reject symlinked databases before SQLite opens its named sidecars. */
    struct stat ds;
    const char *names[] = {"catalog.sqlite", "catalog.sqlite-wal", "catalog.sqlite-shm", NULL};
    for (int i = 0; names[i]; i++)
        if (!fstatat(c->mfd, names[i], &ds, AT_SYMLINK_NOFOLLOW) && !S_ISREG(ds.st_mode)) {
            free(dbpath);
            return fail(c, "Unsafe database file");
        }
    int rc = sqlite3_open_v2(
        dbpath, &c->db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, NULL);
    free(dbpath);
    if (rc != SQLITE_OK)
        return fail(c, "Database open failed");
    sqlite3_busy_timeout(c->db, 10000);
    sqlite3_stmt *v = sql_prepare(c, "PRAGMA user_version");
    int version = sqlite3_step(v) == SQLITE_ROW ? sqlite3_column_int(v, 0) : 0;
    sqlite3_finalize(v);
    if (version > SCHEMA_VERSION)
        return fail(c, "Database is from a newer application; refusing to modify it");
    if (sql_exec(c, schema) || sql_exec(c, group_schema) || sql_exec(c, "PRAGMA user_version=1"))
        return -1;
    char *old = setting(c, "source", "");
    c->source = source ? realpath(source, NULL) : xstrdup(old);
    if (source && !c->source) {
        free(old);
        return fail(c, "Source: %s", strerror(errno));
    }
    if (source && *old && strcmp(old, c->source)) {
        free(old);
        return fail(c, "Source differs from the saved session (%s)", c->source);
    }
    free(old);
    if (!c->source)
        c->source = xstrdup("");
    if (*c->source && !strcmp(c->source, c->destination))
        return fail(c, "Source and destination must differ");
    if (*c->source && (!strcmp(c->destination, "/") || descendant(c->source, c->destination)))
        return fail(c, "Destination must not contain the source tree");
    if (*c->source)
        c->sfd = open(c->source, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (c->sfd < 0 && !browse)
        return fail(c, "Source unavailable; use browse mode for the historical view");
    if (*c->source)
        set_setting(c, "source", c->source);
    set_setting(c, "destination_hint", c->destination);
    c->readonly = browse;
    c->log = activity_open(c->mfd);
    if (!c->log)
        return fail(c, "Cannot open activity.log: %s", strerror(errno));
    activity_log(c, LOG_INFO, "session.open", c->source, c->destination, "CCurate session opened");
    if (recover_operations(c))
        return -1;
    progress(c, browse ? "Historical view" : "Ready");
    return 0;
}
void catalog_close(Catalog *c)
{
    activity_log(c, LOG_INFO, "session.close", NULL, NULL,
                 "Session closing; completed work remains saved");
    activity_close(c->log);
    c->log = NULL;
    if (c->db) {
        sqlite3_wal_checkpoint_v2(c->db, NULL, SQLITE_CHECKPOINT_PASSIVE, NULL, NULL);
        sqlite3_close(c->db);
    }
    if (c->sfd >= 0)
        close(c->sfd);
    if (c->dfd >= 0)
        close(c->dfd);
    if (c->mfd >= 0)
        close(c->mfd);
    if (c->lockfd >= 0)
        close(c->lockfd);
    free(c->source);
    free(c->destination);
    free(c->state_dir);
    pthread_mutex_destroy(&c->progress_mutex);
}

static char *attributes(int fd, char error[256])
{
#ifdef _WIN32
    return cc_win_attributes(fd, error);
#else
    Buf b = {0};
    badd(&b, "[");
    ssize_t n = flistxattr(fd, NULL, 0);
    if (n < 0) {
        if (errno != ENOTSUP)
            snprintf(error, 256, "xattrs: %s", strerror(errno));
        badd(&b, "]");
        return b.s;
    }
    char *names = xmalloc(n + 1);
    if (n && flistxattr(fd, names, n) != n) {
        snprintf(error, 256, "xattrs changed during scan");
        free(names);
        badd(&b, "]");
        return b.s;
    }
    int count = 0;
    for (ssize_t p = 0; p < n;) {
        char *name = names + p;
        size_t k = strlen(name);
        p += k + 1;
        ssize_t z = fgetxattr(fd, name, NULL, 0);
        if (z < 0) {
            snprintf(error, 256, "xattr unreadable: %s", strerror(errno));
            continue;
        }
        unsigned char *v = xmalloc(z + 1);
        if (fgetxattr(fd, name, v, z) != z) {
            free(v);
            snprintf(error, 256, "xattr changed during scan");
            continue;
        }
        char *nb = b64(name, k), *vb = b64(v, z);
        badd(&b, "%s{\"name_b64\":\"%s\",\"value_b64\":\"%s\"}", count++ ? "," : "", nb, vb);
        free(nb);
        free(vb);
        free(v);
    }
    free(names);
    badd(&b, "]");
    return b.s;
#endif
}
static const char *kind_of(mode_t m)
{
    return S_ISREG(m)    ? "file"
           : S_ISDIR(m)  ? "directory"
           : S_ISLNK(m)  ? "symlink"
           : S_ISFIFO(m) ? "fifo"
           : S_ISSOCK(m) ? "socket"
           : S_ISBLK(m)  ? "block"
           : S_ISCHR(m)  ? "character"
                         : "unknown";
}
static int kernel_fs(int fd)
{
#ifdef _WIN32
    (void)fd;
    return 0;
#else
    struct statfs fs;
    return !fstatfs(fd, &fs) &&
           (fs.f_type == PROC_SUPER_MAGIC || fs.f_type == SYSFS_MAGIC ||
            fs.f_type == DEBUGFS_MAGIC || fs.f_type == TRACEFS_MAGIC ||
            fs.f_type == CGROUP_SUPER_MAGIC || fs.f_type == CGROUP2_SUPER_MAGIC);
#endif
}
static int scan_level(const char *error)
{
    return strstr(error, "changed") || strstr(error, "Virtual kernel") || strstr(error, "Cancelled")
               ? LOG_WARNING
               : LOG_ERROR;
}
static int update_error(Catalog *c, const char *p, const char *error)
{
    if (error && *error)
        activity_log(c, scan_level(error), "scan.path", p, NULL, "%s", error);
    sqlite3_stmt *s = sql_prepare(c, "UPDATE entries SET error=? WHERE path=?");
    bind_text(s, 1, error);
    bind_text(s, 2, p);
    int rc = sqlite3_step(s);
    sqlite3_finalize(s);
    return rc == SQLITE_DONE ? 0 : -1;
}
static void clear_discovery(Catalog *c, const char *path)
{
    sqlite3_stmt *q =
        sql_prepare(c, "DELETE FROM discoveries WHERE eid=(SELECT id FROM entries WHERE path=?)");
    bind_text(q, 1, path);
    sqlite3_step(q);
    sqlite3_finalize(q);
}
int scan_one(Catalog *c, const char *path, int full, int64_t generation)
{
    char *leaf = NULL;
    int pfd = *path ? parent_beneath(c->sfd, path, 0, &leaf) : dup(c->sfd);
    struct stat st;
    int ok =
        pfd >= 0 && (*path ? fstatat(pfd, leaf, &st, AT_SYMLINK_NOFOLLOW) : fstat(pfd, &st)) == 0;
    int saved_errno = errno;
    char *par = parent_path(path);
    sqlite3_stmt *s =
        sql_prepare(c, "INSERT INTO entries(path,parent,name,kind,seen) VALUES(?,?,?,?,?) ON "
                       "CONFLICT(path) DO UPDATE SET present=1,seen=excluded.seen");
    bind_text(s, 1, path);
    bind_text(s, 2, *path ? par : NULL);
    bind_text(s, 3, base_name(path));
    bind_text(s, 4, ok ? kind_of(st.st_mode) : "unknown");
    sqlite3_bind_int64(s, 5, generation);
    int rc = sqlite3_step(s);
    sqlite3_finalize(s);
    free(par);
    if (rc != SQLITE_DONE) {
        free(leaf);
        if (pfd >= 0)
            close(pfd);
        return fail(c, "Scan database: %s", sqlite3_errmsg(c->db));
    }
    if (!ok) {
        update_error(c, path, strerror(saved_errno));
        free(leaf);
        if (pfd >= 0)
            close(pfd);
        return -1;
    }
    s = sql_prepare(
        c, "SELECT e.id,e.current,v.dev,v.ino,v.size,v.mode,v.mtime,v.ctime,v.hash,e.error FROM "
           "entries e LEFT JOIN versions v ON v.id=e.current WHERE e.path=?");
    bind_text(s, 1, path);
    sqlite3_step(s);
    int64_t eid = sqlite3_column_int64(s, 0), old = sqlite3_column_int64(s, 1);
    int same =
        old && sqlite3_column_int64(s, 2) == (int64_t)st.st_dev &&
        sqlite3_column_int64(s, 3) == (int64_t)st.st_ino &&
        sqlite3_column_int64(s, 4) == st.st_size && sqlite3_column_int64(s, 5) == st.st_mode &&
        sqlite3_column_int64(s, 6) == mtime_ns(&st) && sqlite3_column_int64(s, 7) == ctime_ns(&st);
    if (S_ISREG(st.st_mode) && (!sqlite3_column_text(s, 8) || !*(char *)sqlite3_column_text(s, 8)))
        same = 0;
    if (sqlite3_column_text(s, 9) && *(char *)sqlite3_column_text(s, 9))
        same = 0;
    char old_hash[129] = {0};
    if (sqlite3_column_text(s, 8))
        snprintf(old_hash, sizeof(old_hash), "%s", sqlite3_column_text(s, 8));
    sqlite3_finalize(s);
    if (same && !full) {
        free(leaf);
        close(pfd);
        clear_discovery(c, path);
        return 0;
    }
    int fd = -1;
    char hash[129] = {0}, err[1024] = {0}, meta_error[256] = {0};
    char *link = NULL, *attrs = xstrdup("[]");
    int64_t born = 0;
    struct statx sx;
    if (statx(pfd, *path ? leaf : "", AT_SYMLINK_NOFOLLOW | (*path ? 0 : AT_EMPTY_PATH),
              STATX_BTIME, &sx) == 0 &&
        (sx.stx_mask & STATX_BTIME))
        born = (int64_t)sx.stx_btime.tv_sec * 1000000000 + sx.stx_btime.tv_nsec;
    if (S_ISREG(st.st_mode) && kernel_fs(pfd)) {
        snprintf(err, sizeof(err), "Virtual kernel file: inventoried, content not read");
    } else if (S_ISREG(st.st_mode) || S_ISDIR(st.st_mode)) {
        fd = *path ? openat(pfd, leaf,
                            O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_NOATIME | O_CLOEXEC |
                                (S_ISDIR(st.st_mode) ? O_DIRECTORY : 0))
                   : dup(pfd);
        if (fd < 0 && errno == EPERM)
            fd = openat(pfd, leaf,
                        O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC |
                            (S_ISDIR(st.st_mode) ? O_DIRECTORY : 0));
        if (fd < 0)
            snprintf(err, sizeof(err), "open: %s", strerror(errno));
        else {
            struct stat actual;
            if (fstat(fd, &actual) || !stat_same(&st, &actual))
                snprintf(err, sizeof(err), "File changed before opening");
            else {
                free(attrs);
                attrs = attributes(fd, meta_error);
#ifdef _WIN32
                if (!cc_win_content_supported(fd, err, sizeof(err))) {
                    /* Named-stream bytes must never be silently marked secured. */
                } else
#endif
                    if (S_ISREG(st.st_mode) && hash_fd(c, fd, hash, NULL))
                    snprintf(err, sizeof(err), "%s", c->error);
            }
            close(fd);
        }
    } else if (S_ISLNK(st.st_mode)) {
        size_t capacity = (size_t)st.st_size + 4096;
        link = xmalloc(capacity);
        ssize_t n = readlinkat(pfd, leaf, link, capacity - 1);
        if (n < 0)
            snprintf(err, sizeof(err), "readlink: %s", strerror(errno));
        else
            link[n] = 0;
    }
    free(leaf);
    close(pfd);
    if (same && full && !*err && !*meta_error && !strcmp(hash, old_hash)) {
        free(link);
        free(attrs);
        clear_discovery(c, path);
        return 0;
    }
    s = sql_prepare(c, "INSERT INTO "
                       "versions(eid,mode,size,uid,gid,mtime,ctime,atime,btime,dev,ino,hash,link,"
                       "attrs,meta_error) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
    sqlite3_bind_int64(s, 1, eid);
    sqlite3_bind_int64(s, 2, st.st_mode);
    sqlite3_bind_int64(s, 3, st.st_size);
    sqlite3_bind_int64(s, 4, st.st_uid);
    sqlite3_bind_int64(s, 5, st.st_gid);
    sqlite3_bind_int64(s, 6, mtime_ns(&st));
    sqlite3_bind_int64(s, 7, ctime_ns(&st));
    sqlite3_bind_int64(s, 8, (int64_t)st.st_atim.tv_sec * 1000000000 + st.st_atim.tv_nsec);
    sqlite3_bind_int64(s, 9, born);
    sqlite3_bind_int64(s, 10, st.st_dev);
    sqlite3_bind_int64(s, 11, st.st_ino);
    bind_text(s, 12, *hash ? hash : NULL);
    bind_text(s, 13, link);
    bind_text(s, 14, attrs);
    bind_text(s, 15, meta_error);
    rc = sqlite3_step(s);
    sqlite3_finalize(s);
    free(link);
    free(attrs);
    if (rc != SQLITE_DONE)
        return fail(c, "Version insert failed");
    int64_t vid = sqlite3_last_insert_rowid(c->db);
    s = sql_prepare(
        c,
        "UPDATE entries SET current=?,kind=?,error=?,reviewed=CASE WHEN current IS ? THEN reviewed "
        "ELSE 0 END,discarded=CASE WHEN current IS ? THEN discarded ELSE 0 END WHERE id=?");
    sqlite3_bind_int64(s, 1, vid);
    bind_text(s, 2, kind_of(st.st_mode));
    bind_text(s, 3, err);
    sqlite3_bind_int64(s, 4, vid);
    sqlite3_bind_int64(s, 5, vid);
    sqlite3_bind_int64(s, 6, eid);
    sqlite3_step(s);
    sqlite3_finalize(s);
    if (*hash) {
        s = sql_prepare(c, "INSERT OR IGNORE INTO objects(hash,size) VALUES(?,?)");
        bind_text(s, 1, hash);
        sqlite3_bind_int64(s, 2, st.st_size);
        sqlite3_step(s);
        sqlite3_finalize(s);
        s = sql_prepare(c, "UPDATE versions SET accepted=1 WHERE id=? AND EXISTS(SELECT 1 FROM "
                           "objects WHERE hash=versions.hash AND state='verified')");
        sqlite3_bind_int64(s, 1, vid);
        sqlite3_step(s);
        int kept = sqlite3_changes(c->db);
        sqlite3_finalize(s);
        if (kept)
            preserve_parents(c, path);
    }
    if (*err)
        activity_log(c, scan_level(err), "scan.read", path, NULL, "%s", err);
    if (*meta_error)
        activity_log(c, LOG_WARNING, "scan.metadata", path, NULL, "%s", meta_error);
    if (!*err)
        clear_discovery(c, path);
    atomic_fetch_add(&c->files, 1);
    return *err ? -1 : 0;
}

static _Thread_local unsigned scan_steps;
/* The plan is a disposable table in the destination catalog, never a RAM array of a whole disk.
 * An estimate is deliberately unavailable until this enumeration has finished. */
static int plan_one(Catalog *c, const char *path, int full, int64_t generation, int *directory)
{
    char *leaf = NULL;
    int parent = *path ? parent_beneath(c->sfd, path, 0, &leaf) : dup(c->sfd);
    struct stat st = {0};
    int ok = parent >= 0 &&
             (*path ? fstatat(parent, leaf, &st, AT_SYMLINK_NOFOLLOW) : fstat(parent, &st)) == 0;
    int saved_errno = errno;
    *directory = ok && S_ISDIR(st.st_mode);
    int regular = ok && S_ISREG(st.st_mode) && !kernel_fs(parent), same = 0, has_hash = 0,
        had_error = 0;
    sqlite3_stmt *q =
        sql_prepare(c, "SELECT v.dev,v.ino,v.size,v.mode,v.mtime,v.ctime,v.hash,e.error "
                       "FROM entries e JOIN versions v ON v.id=e.current WHERE e.path=?");
    bind_text(q, 1, path);
    if (sqlite3_step(q) == SQLITE_ROW && ok) {
        same = sqlite3_column_int64(q, 0) == (int64_t)st.st_dev &&
               sqlite3_column_int64(q, 1) == (int64_t)st.st_ino &&
               sqlite3_column_int64(q, 2) == st.st_size &&
               sqlite3_column_int64(q, 3) == st.st_mode &&
               sqlite3_column_int64(q, 4) == mtime_ns(&st) &&
               sqlite3_column_int64(q, 5) == ctime_ns(&st);
        const char *hash = (char *)sqlite3_column_text(q, 6),
                   *error = (char *)sqlite3_column_text(q, 7);
        has_hash = hash && *hash;
        had_error = error && *error;
    }
    sqlite3_finalize(q);
    int needs = regular && (full || !same || !has_hash || had_error);
    /* Publish observed names and basic metadata immediately, without inventing a
     * verified version or changing retained history before content is read. */
    char *par = parent_path(path);
    q = sql_prepare(c, "INSERT INTO entries(path,parent,name,kind,seen) VALUES(?,?,?,?,?) "
                       "ON CONFLICT(path) DO UPDATE SET present=1,seen=excluded.seen RETURNING id");
    bind_text(q, 1, path);
    bind_text(q, 2, *path ? par : NULL);
    bind_text(q, 3, base_name(path));
    bind_text(q, 4, ok ? kind_of(st.st_mode) : "unknown");
    sqlite3_bind_int64(q, 5, generation);
    int step = sqlite3_step(q);
    int64_t eid = step == SQLITE_ROW ? sqlite3_column_int64(q, 0) : 0;
    sqlite3_finalize(q);
    free(par);
    if (!eid) {
        if (parent >= 0)
            close(parent);
        free(leaf);
        return fail(c, "Cannot publish scan inventory");
    }
    if (ok && (!same || needs || had_error)) {
        struct statx sx;
        int64_t born = 0;
        if (!statx(parent, *path ? leaf : "", AT_SYMLINK_NOFOLLOW | (*path ? 0 : AT_EMPTY_PATH),
                   STATX_BTIME, &sx) &&
            (sx.stx_mask & STATX_BTIME))
            born = (int64_t)sx.stx_btime.tv_sec * 1000000000 + sx.stx_btime.tv_nsec;
        int64_t values[] = {eid,       st.st_size, mtime_ns(&st), born,     st.st_mode,
                            st.st_uid, st.st_gid,  st.st_dev,     st.st_ino};
        q = sql_prepare(c, "INSERT OR REPLACE INTO discoveries VALUES(?,?,?,?,?,?,?,?,?)");
        for (int i = 0; i < 9; i++)
            sqlite3_bind_int64(q, i + 1, values[i]);
        step = sqlite3_step(q);
        sqlite3_finalize(q);
        if (step != SQLITE_DONE) {
            if (parent >= 0)
                close(parent);
            free(leaf);
            return fail(c, "Cannot publish scan metadata");
        }
        if (!same) {
            q = sql_prepare(c, "UPDATE entries SET reviewed=0,discarded=0 WHERE id=?");
            sqlite3_bind_int64(q, 1, eid);
            sqlite3_step(q);
            sqlite3_finalize(q);
        }
        update_error(c, path, "");
    } else if (!ok)
        update_error(c, path, strerror(saved_errno));
    if (parent >= 0)
        close(parent);
    free(leaf);
    q = sql_prepare(c, "INSERT INTO scan_queue(path,bytes,hashing) VALUES(?,?,?)");
    bind_text(q, 1, path);
    sqlite3_bind_int64(q, 2, needs ? st.st_size : 0);
    sqlite3_bind_int(q, 3, needs);
    step = sqlite3_step(q);
    sqlite3_finalize(q);
    if (step != SQLITE_DONE)
        return fail(c, "Cannot store scan plan (check destination space)");
    atomic_fetch_add(&c->scan.found, 1);
    if (needs) {
        atomic_fetch_add(&c->scan.planned_bytes, st.st_size);
        atomic_fetch_add(&c->scan.hash_files, 1);
    }
    return 0;
}
typedef struct Ancestor {
    dev_t dev;
    ino_t ino;
    const struct Ancestor *up;
} Ancestor;
static int name_cmp(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}
static void plan_error(Catalog *c, const char *path, const char *error)
{
    sqlite3_stmt *q = sql_prepare(c, "UPDATE scan_queue SET error=? WHERE path=?");
    bind_text(q, 1, error);
    bind_text(q, 2, path);
    sqlite3_step(q);
    sqlite3_finalize(q);
    update_error(c, path, error);
}
static int walk(Catalog *c, const char *path, int full, int64_t gen, const Ancestor *up, int depth)
{
    if (atomic_load(&c->cancel))
        return -1;
    progress(c, "Listing files: %s", path);
    int directory = 0;
    int rc = plan_one(c, path, full, gen, &directory);
    if (rc)
        return rc;
    if (++scan_steps % 128 == 0) {
        if (sql_exec(c, "COMMIT") || sql_exec(c, "BEGIN IMMEDIATE"))
            return -1;
    }
    if (!directory)
        return rc;
    int fd = open_beneath(c->sfd, path, O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0) {
        plan_error(c, path, strerror(errno));
        return -1;
    }
    struct stat st;
    if (fstat(fd, &st)) {
        plan_error(c, path, strerror(errno));
        close(fd);
        return -1;
    }
    if (depth > 512) {
        close(fd);
        plan_error(c, path, "Directory nesting limit reached");
        return -1;
    }
    for (const Ancestor *a = up; a; a = a->up)
        if (a->dev == st.st_dev && a->ino == st.st_ino) {
            close(fd);
            plan_error(c, path, "Directory mount cycle");
            return -1;
        }
    struct stat target;
    fstat(c->dfd, &target);
    if (st.st_dev == target.st_dev && st.st_ino == target.st_ino) {
        close(fd);
        plan_error(c, path, "Destination mount: not traversed");
        return -1;
    }
    Ancestor here = {st.st_dev, st.st_ino, up};
    DIR *d = fdopendir(fd);
    if (!d) {
        plan_error(c, path, strerror(errno));
        close(fd);
        return -1;
    }
    char **names = NULL;
    size_t n = 0, cap = 0;
    struct dirent *e;
    errno = 0;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 64;
            names = realloc(names, cap * sizeof(*names));
            if (!names)
                abort();
        }
        names[n++] = xstrdup(e->d_name);
    }
    if (errno) {
        plan_error(c, path, strerror(errno));
        rc = -1;
    }
    closedir(d);
    if (n > 1)
        qsort(names, n, sizeof(*names), name_cmp);
    for (size_t i = 0; i < n; i++) {
        if (!atomic_load(&c->cancel)) {
            char *child = join_path(path, names[i]);
            if (walk(c, child, full, gen, &here, depth + 1))
                rc = -1;
            free(child);
        } else
            rc = -1;
        free(names[i]);
    }
    free(names);
    return rc;
}
int scan_source(Catalog *c, int full)
{
    if (c->sfd < 0)
        return fail(c, "Source offline");
    activity_log(c, LOG_INFO, "scan.start", c->source, NULL,
                 full ? "Full SHA-512 scan started" : "Incremental scan started");
    atomic_store(&c->files, 0);
    atomic_store(&c->bytes, 0);
    atomic_store(&c->scan.phase, SCAN_ENUMERATE);
    atomic_store(&c->scan.found, 0);
    atomic_store(&c->scan.done, 0);
    atomic_store(&c->scan.planned_bytes, 0);
    atomic_store(&c->scan.resolved_bytes, 0);
    atomic_store(&c->scan.current_bytes, 0);
    atomic_store(&c->scan.current_plan, 0);
    atomic_store(&c->scan.hash_files, 0);
    atomic_store(&c->scan.hashed_files, 0);
    atomic_store(&c->scan.errors, 0);
    atomic_store(&c->scan.finished, 0);
    atomic_store(&c->scan.started, monotonic_ns());
    atomic_store(&c->scan.phase_started, monotonic_ns());
    int64_t gen = now_ns();
    char g[32];
    snprintf(g, sizeof(g), "%lld", (long long)gen);
    set_setting(c, "scan_started", g);
    set_setting(c, "scan_complete", "0");
    scan_steps = 0;
    progress(c, "Preparing the occurrence index...");
    if (prepare_hash_groups(c)) {
        atomic_store(&c->scan.phase, SCAN_STOPPED);
        return -1;
    }
    if (sql_exec(c, "DROP TABLE IF EXISTS scan_queue; "
                    "CREATE TABLE scan_queue(id INTEGER PRIMARY KEY,path TEXT UNIQUE,bytes "
                    "INTEGER,hashing INTEGER,error TEXT NOT NULL DEFAULT '');"))
        return -1;
    if (sql_exec(c, "BEGIN IMMEDIATE"))
        return -1;
    int rc = walk(c, "", full, gen, NULL, 0);
    if (sql_exec(c, "COMMIT"))
        rc = -1;
    int enumeration_ok = !rc;
    activity_log(c, rc ? LOG_WARNING : LOG_INFO, "scan.enumerated", NULL, NULL,
                 "Discovered %llu entries; planned %llu bytes for hashing",
                 (unsigned long long)atomic_load(&c->scan.found),
                 (unsigned long long)atomic_load(&c->scan.planned_bytes));
    atomic_store(&c->scan.phase_started, monotonic_ns());
    atomic_store(&c->scan.phase, SCAN_HASH);
    int64_t cursor = 0;
    /* Finish one entry before committing it; a huge file does not hide all preceding
     * progress. Reads and checkpoints remain on this worker, not the UI thread. */
    while (!atomic_load(&c->cancel)) {
        sqlite3_stmt *q = sql_prepare(
            c,
            "SELECT id,path,bytes,hashing,error FROM scan_queue WHERE id>? ORDER BY id LIMIT 128");
        sqlite3_bind_int64(q, 1, cursor);
        char *paths[128], *errors[128];
        int64_t ids[128], sizes[128];
        int hashing[128], n = 0;
        while (sqlite3_step(q) == SQLITE_ROW && n < 128) {
            ids[n] = sqlite3_column_int64(q, 0);
            paths[n] = xstrdup((char *)sqlite3_column_text(q, 1));
            sizes[n] = sqlite3_column_int64(q, 2);
            hashing[n] = sqlite3_column_int(q, 3);
            errors[n] = xstrdup((char *)sqlite3_column_text(q, 4));
            n++;
        }
        sqlite3_finalize(q);
        if (!n)
            break;
        if (sql_exec(c, "BEGIN IMMEDIATE")) {
            for (int i = 0; i < n; i++) {
                free(paths[i]);
                free(errors[i]);
            }
            rc = -1;
            break;
        }
        for (int i = 0; i < n; i++) {
            if (!atomic_load(&c->cancel)) {
                atomic_store(&c->scan.current_bytes, 0);
                atomic_store(&c->scan.current_plan, sizes[i]);
                progress(c, "SHA-512 / metadata: %s", paths[i]);
                int entry_error = scan_one(c, paths[i], full, gen);
                if (*errors[i]) {
                    update_error(c, paths[i], errors[i]);
                    entry_error = -1;
                }
                if (entry_error) {
                    rc = -1;
                    atomic_fetch_add(&c->scan.errors, 1);
                }
                atomic_fetch_add(&c->scan.done, 1);
                atomic_fetch_add(&c->scan.resolved_bytes, sizes[i]);
                atomic_store(&c->scan.current_bytes, 0);
                atomic_store(&c->scan.current_plan, 0);
                if (hashing[i])
                    atomic_fetch_add(&c->scan.hashed_files, 1);
                cursor = ids[i];
            }
            free(paths[i]);
            free(errors[i]);
        }
        if (sql_exec(c, "COMMIT")) {
            rc = -1;
            break;
        }
    }
    if (atomic_load(&c->cancel))
        rc = fail(c, "Scan cancelled");
    if (!enumeration_ok) {
        rc = -1;
        atomic_fetch_add(&c->scan.errors, 1);
    }
    sql_exec(c, "DROP TABLE scan_queue");
    /* An unreadable directory must never make its unseen children look deleted. */
    if (!rc) {
        sqlite3_stmt *s = sql_prepare(c, "UPDATE entries SET present=0 WHERE seen<>?");
        sqlite3_bind_int64(s, 1, gen);
        sqlite3_step(s);
        sqlite3_finalize(s);
        set_setting(c, "scan_complete", "1");
    }
    event_log(c, full ? "scan_full" : "scan_incremental", NULL, NULL, NULL);
    atomic_store(&c->scan.phase, SCAN_EXPORT);
    if (export_archive(c)) {
        atomic_store(&c->scan.phase, SCAN_STOPPED);
        return -1;
    }
    atomic_store(&c->scan.finished, monotonic_ns());
    atomic_store(&c->scan.phase, rc ? SCAN_STOPPED : SCAN_DONE);
    activity_log(c, rc ? LOG_WARNING : LOG_INFO, "scan.finish", NULL, NULL,
                 "Processed %llu of %llu entries; errors=%llu; %s",
                 (unsigned long long)atomic_load(&c->scan.done),
                 (unsigned long long)atomic_load(&c->scan.found),
                 (unsigned long long)atomic_load(&c->scan.errors),
                 rc ? "incomplete or cancelled" : "complete");
    progress(c, rc ? "Scan finished with errors / cancelled" : "Scan complete");
    return rc;
}

static const char *entry_columns =
    "e.id,v.id,v.size,v.mtime,v.btime,v.mode,v.uid,v.gid,v.dev,v.ino,e.path,e.parent,e.name,(CASE "
    "(v.mode&61440) WHEN 32768 THEN 'file' WHEN 16384 THEN 'directory' WHEN 40960 THEN 'symlink' "
    "WHEN 4096 THEN 'fifo' WHEN 49152 THEN 'socket' WHEN 24576 THEN 'block' WHEN 8192 THEN "
    "'character' ELSE e.kind "
    "END),v.hash,o.dest,o.state,e.error,e.present,e.reviewed,e.discarded,v.accepted,(SELECT "
    "occurrences FROM hash_groups WHERE hash=v.hash),(SELECT MIN(x.eid) FROM versions x WHERE "
    "x.hash=v.hash)";
static void row_entry(sqlite3_stmt *s, Entry *e)
{
    memset(e, 0, sizeof(*e));
    e->id = sqlite3_column_int64(s, 0);
    e->version = sqlite3_column_int64(s, 1);
    e->size = sqlite3_column_int64(s, 2);
    e->mtime = sqlite3_column_int64(s, 3);
    e->btime = sqlite3_column_int64(s, 4);
    e->mode = sqlite3_column_int64(s, 5);
    e->uid = sqlite3_column_int64(s, 6);
    e->gid = sqlite3_column_int64(s, 7);
    e->dev = sqlite3_column_int64(s, 8);
    e->ino = sqlite3_column_int64(s, 9);
    char **str[] = {&e->path, &e->parent, &e->name,  &e->kind,
                    &e->hash, &e->dest,   &e->state, &e->error};
    for (int i = 0; i < 8; i++)
        *str[i] = xstrdup((char *)sqlite3_column_text(s, 10 + i));
    e->present = sqlite3_column_int(s, 18);
    e->reviewed = sqlite3_column_int(s, 19);
    e->discarded = sqlite3_column_int(s, 20);
    e->accepted = sqlite3_column_int(s, 21);
    e->copies = sqlite3_column_int(s, 22);
    e->original = e->id == sqlite3_column_int64(s, 23);
}
/* Overlay unverified observations only in live views. Retained versions remain
 * immutable and independently browsable if a scan is cancelled. */
static void observed_entry(Catalog *c, Entry *e)
{
    sqlite3_stmt *q =
        sql_prepare(c, "SELECT size,mtime,btime,mode,uid,gid,dev,ino FROM discoveries WHERE eid=?");
    sqlite3_bind_int64(q, 1, e->id);
    if (sqlite3_step(q) == SQLITE_ROW) {
        e->size = sqlite3_column_int64(q, 0);
        e->mtime = sqlite3_column_int64(q, 1);
        e->btime = sqlite3_column_int64(q, 2);
        e->mode = sqlite3_column_int64(q, 3);
        e->uid = sqlite3_column_int64(q, 4);
        e->gid = sqlite3_column_int64(q, 5);
        e->dev = sqlite3_column_int64(q, 6);
        e->ino = sqlite3_column_int64(q, 7);
        free(e->kind);
        e->kind = xstrdup(kind_of(e->mode));
        free(e->hash);
        e->hash = xstrdup("");
        free(e->dest);
        e->dest = xstrdup("");
        free(e->state);
        e->state = xstrdup("pending");
        e->pending = 1;
        e->accepted = 0;
        e->copies = 0;
        e->version = 0;
    }
    sqlite3_finalize(q);
}
void entry_free(Entry *e)
{
    free(e->path);
    free(e->parent);
    free(e->name);
    free(e->kind);
    free(e->hash);
    free(e->dest);
    free(e->state);
    free(e->error);
    memset(e, 0, sizeof(*e));
}
void entries_free(Entry *e, int n)
{
    for (int i = 0; i < n; i++)
        entry_free(e + i);
    free(e);
}
int load_entry(Catalog *c, const char *path, Entry *out, int historical)
{
    char *sql =
        fmt("SELECT %s FROM entries e LEFT JOIN versions v ON v.id=%s LEFT JOIN objects o ON "
            "o.hash=v.hash WHERE e.path=?",
            entry_columns,
            historical ? "(SELECT MAX(h.id) FROM versions h WHERE h.eid=e.id AND h.accepted=1)"
                       : "e.current");
    sqlite3_stmt *s = sql_prepare(c, sql);
    free(sql);
    bind_text(s, 1, path);
    int rc = sqlite3_step(s);
    if (rc == SQLITE_ROW)
        row_entry(s, out);
    sqlite3_finalize(s);
    if (rc == SQLITE_ROW && !historical)
        observed_entry(c, out);
    return rc == SQLITE_ROW ? 0 : fail(c, "Path not catalogued: %s", path);
}
int load_version(Catalog *c, const char *path, int64_t version, Entry *out)
{
    char *sql = fmt("SELECT %s FROM entries e JOIN versions v ON v.id=? AND v.eid=e.id LEFT JOIN "
                    "objects o ON o.hash=v.hash WHERE e.path=?",
                    entry_columns);
    sqlite3_stmt *s = sql_prepare(c, sql);
    free(sql);
    sqlite3_bind_int64(s, 1, version);
    bind_text(s, 2, path);
    int rc = sqlite3_step(s);
    if (rc == SQLITE_ROW)
        row_entry(s, out);
    sqlite3_finalize(s);
    return rc == SQLITE_ROW ? 0 : -1;
}
/* Apply filters before LIMIT: filtered pages must not hide matches beyond the first
 * thousand rows. COUNT and page queries share this predicate. */
static char *listing_from(int historical, int unsecured, int fast)
{
    Buf b = {0};
    badd(&b,
         "FROM entries e LEFT JOIN versions v ON v.id=%s LEFT JOIN objects o ON o.hash=v.hash "
         "WHERE e.parent=? AND e.path<>'' AND (?='' OR instr(lower(e.name),lower(?))>0)",
         historical ? "(SELECT MAX(h.id) FROM versions h WHERE h.eid=e.id AND h.accepted=1)"
                    : "e.current");
    if (historical)
        badd(&b, " AND (v.accepted=1 OR (e.kind='directory' AND EXISTS (SELECT 1 FROM entries x "
                 "JOIN versions h ON h.eid=x.id AND h.accepted=1 WHERE x.path>=e.path||'/' "
                 "AND x.path<e.path||'0')))");
    if (unsecured) {
        const char *pending =
            historical ? "0" : "EXISTS (SELECT 1 FROM discoveries d WHERE d.eid=e.id)";
        const char *child_pending =
            historical ? "0" : "EXISTS (SELECT 1 FROM discoveries d WHERE d.eid=x.id)";
        badd(
            &b,
            " AND e.discarded=0 AND (%s OR e.kind='directory' OR (COALESCE(o.state,'')<>'verified' "
            "AND (COALESCE(v.hash,'')<>'' OR COALESCE(v.accepted,0)=0)))",
            pending);
        if (!fast)
            badd(&b,
                 " AND (%s OR e.kind<>'directory' OR COALESCE(v.accepted,0)=0 OR EXISTS ("
                 "SELECT 1 FROM entries x LEFT JOIN versions h ON h.id=x.current LEFT JOIN "
                 "objects t ON t.hash=h.hash WHERE x.path>=e.path||'/' AND x.path<e.path||'0' "
                 "AND x.discarded=0 AND x.present=1 AND (%s OR NOT "
                 "((COALESCE(t.state,'')='verified' "
                 "AND h.hash IS NOT NULL) OR (COALESCE(h.accepted,0)=1 AND x.kind<>'file')))))",
                 pending, child_pending);
    }
    return b.s;
}
int64_t count_entries(Catalog *c, const char *path, int historical, int unsecured,
                      const char *search, int fast)
{
    char *from = historical || unsecured
                     ? listing_from(historical, unsecured, fast)
                     : xstrdup("FROM entries e WHERE e.parent=? AND e.path<>'' AND "
                               "(?='' OR instr(lower(e.name),lower(?))>0)");
    char *sql = fmt("SELECT COUNT(*) %s", from);
    sqlite3_stmt *q = sql_prepare(c, sql);
    free(sql);
    free(from);
    bind_text(q, 1, path);
    bind_text(q, 2, search ? search : "");
    bind_text(q, 3, search ? search : "");
    int64_t count = sqlite3_step(q) == SQLITE_ROW ? sqlite3_column_int64(q, 0) : 0;
    sqlite3_finalize(q);
    return count;
}
/* Only fixed SQL expressions are used as sort keys. Values remain parameters.
 * Directory grouping and the final binary name tie-breaker are deterministic. */
static char *listing_order(int historical, int fast, int sort)
{
    int column = sort & SORT_COLUMN_MASK;
    const char *direction = sort & SORT_DESC ? "DESC" : "ASC";
    const char *pending = historical ? "0" : "EXISTS(SELECT 1 FROM discoveries d WHERE d.eid=e.id)";
    const char *secured = "(o.state='verified' OR (COALESCE(v.hash,'')='' AND v.accepted=1))";
    const char *copies = "COALESCE((SELECT occurrences FROM hash_groups WHERE hash=v.hash),0)";
    const char *directory =
        historical ? "COALESCE((v.mode & 61440)=16384,e.kind='directory')" : "(e.kind='directory')";
    char *key = NULL;
    if (column == SORT_STATUS) {
        const char *dir_ok =
            "v.accepted=1 AND NOT EXISTS(SELECT 1 FROM entries x "
            "LEFT JOIN versions h ON h.id=x.current LEFT JOIN objects t ON t.hash=h.hash "
            "WHERE x.path>=e.path||'/' AND x.path<e.path||'0' AND x.discarded=0 AND x.present=1 "
            "AND (EXISTS(SELECT 1 FROM discoveries d WHERE d.eid=x.id) OR NOT "
            "((COALESCE(t.state,'')='verified' AND h.hash IS NOT NULL) OR "
            "(COALESCE(h.accepted,0)=1 AND x.kind<>'file'))))";
        char *directory_status =
            fast
                ? xstrdup("'scanning...'")
                : fmt("CASE WHEN e.discarded THEN 'REJ' WHEN COALESCE(e.error,'')<>'' THEN 'ERROR' "
                      "WHEN %s THEN 'OK' ELSE '--' END",
                      dir_ok);
        key = fmt(
            "CASE WHEN %s THEN CASE WHEN COALESCE(e.error,'')<>'' THEN 'ERROR' ELSE 'PENDING' END "
            "WHEN %s THEN %s "
            "WHEN e.discarded THEN 'REJECTED' WHEN COALESCE(e.error,'')<>'' THEN 'ERROR' "
            "WHEN o.state='corrupt' THEN 'CHANGED' "
            "WHEN o.state='missing' AND COALESCE(o.dest,'')<>'' THEN 'MISSING' "
            "WHEN o.state='unchecked' THEN 'UNCHECKED' WHEN %s THEN 'OK' "
            "WHEN %s>1 THEN '--' ELSE 'UNCOPIED' END",
            pending, directory, directory_status, secured, copies);
        free(directory_status);
    } else if (column == SORT_SIZE || column == SORT_MTIME || column == SORT_BTIME ||
               column == SORT_MODE) {
        const char *field = column == SORT_SIZE    ? "size"
                            : column == SORT_MTIME ? "mtime"
                            : column == SORT_BTIME ? "btime"
                                                   : "mode";
        key = historical ? fmt("COALESCE(v.%s,0)", field)
                         : fmt("COALESCE((SELECT d.%s FROM discoveries d WHERE d.eid=e.id),v.%s,0)",
                               field, field);
        if (column == SORT_MODE) {
            char *masked = fmt("(%s & 4095)", key);
            free(key);
            key = masked;
        }
    } else
        key = xstrdup("e.name COLLATE BINARY");
    char *tie = column == SORT_STATUS
                    ? fmt(",CASE WHEN %s THEN 0 ELSE %s END %s", pending, copies, direction)
                    : xstrdup("");
    char *order = fmt("%s DESC,%s %s%s,e.name COLLATE BINARY ASC", directory, key, direction, tie);
    free(key);
    free(tie);
    return order;
}
int64_t entry_position(Catalog *c, const char *parent, const char *anchor, int historical,
                       int unsecured, const char *search, int fast, int sort)
{
    if (!anchor || !*anchor)
        return -1;
    if (!historical && !unsecured && (sort & SORT_COLUMN_MASK) == SORT_NAME) {
        /* The common path uses the listing index, without sorting or joining all
         * versions just to find one directory among millions of siblings. */
        char *rank_sql =
            fmt("SELECT (SELECT COUNT(*) FROM entries e WHERE e.parent=a.parent AND e.path<>'' "
                "AND (?='' OR instr(lower(e.name),lower(?))>0) AND "
                "((e.kind='directory')>(a.kind='directory') OR "
                "((e.kind='directory')=(a.kind='directory') AND e.name%sa.name COLLATE BINARY))) "
                "FROM entries a WHERE a.parent=? AND a.path=? AND "
                "(?='' OR instr(lower(a.name),lower(?))>0)",
                sort & SORT_DESC ? ">" : "<");
        sqlite3_stmt *q = sql_prepare(c, rank_sql);
        free(rank_sql);
        bind_text(q, 1, search ? search : "");
        bind_text(q, 2, search ? search : "");
        bind_text(q, 3, parent);
        bind_text(q, 4, anchor);
        bind_text(q, 5, search ? search : "");
        bind_text(q, 6, search ? search : "");
        int64_t position = sqlite3_step(q) == SQLITE_ROW ? sqlite3_column_int64(q, 0) : -1;
        sqlite3_finalize(q);
        return position;
    }
    char *from = listing_from(historical, unsecured, fast);
    char *order = listing_order(historical, fast, sort);
    char *sql = fmt("SELECT position FROM (SELECT e.path,ROW_NUMBER() OVER (ORDER BY %s)-1 AS "
                    "position %s) WHERE path=?",
                    order, from);
    free(order);
    sqlite3_stmt *q = sql_prepare(c, sql);
    free(sql);
    free(from);
    bind_text(q, 1, parent);
    bind_text(q, 2, search ? search : "");
    bind_text(q, 3, search ? search : "");
    bind_text(q, 4, anchor);
    int64_t position = sqlite3_step(q) == SQLITE_ROW ? sqlite3_column_int64(q, 0) : -1;
    sqlite3_finalize(q);
    return position;
}
int list_entries_page(Catalog *c, const char *path, Entry **out, int historical, int unsecured,
                      const char *search, int page, int limit, int fast, int sort, int64_t *total)
{
    if (total)
        *total = count_entries(c, path, historical, unsecured, search, fast);
    char *from = listing_from(historical, unsecured, fast);
    char *order = listing_order(historical, fast, sort);
    char *sql = fmt("SELECT %s %s ORDER BY %s LIMIT ? OFFSET ?", entry_columns, from, order);
    free(order);
    free(from);
    sqlite3_stmt *s = sql_prepare(c, sql);
    free(sql);
    bind_text(s, 1, path);
    bind_text(s, 2, search ? search : "");
    bind_text(s, 3, search ? search : "");
    sqlite3_bind_int(s, 4, limit ? limit : -1);
    sqlite3_bind_int64(s, 5, (int64_t)page * limit);
    Entry *rows = NULL;
    int n = 0, cap = 0;
    while (sqlite3_step(s) == SQLITE_ROW) {
        Entry e;
        row_entry(s, &e);
        if (!historical)
            observed_entry(c, &e);
        int dir = !strcmp(e.kind, "directory");
        if (dir && !fast) {
            sqlite3_stmt *d = sql_prepare(
                c, "SELECT COUNT(*),COALESCE(SUM(CASE WHEN NOT EXISTS(SELECT 1 FROM discoveries d "
                   "WHERE d.eid=e.id) AND ((o.state='verified' AND v.hash IS NOT "
                   "NULL) OR (v.accepted=1 AND e.kind<>'file')) THEN 1 ELSE 0 "
                   "END),0),COALESCE(SUM(v.accepted),0) FROM entries e LEFT JOIN versions v ON "
                   "v.id=e.current LEFT JOIN objects o ON o.hash=v.hash WHERE e.path>=?||'/' AND "
                   "e.path<?||'0' AND e.discarded=0 AND e.present=1");
            bind_text(d, 1, e.path);
            bind_text(d, 2, e.path);
            if (sqlite3_step(d) == SQLITE_ROW) {
                e.children = sqlite3_column_int64(d, 0);
                e.secured = sqlite3_column_int64(d, 1);
            }
            sqlite3_finalize(d);
        }
        if (dir && fast)
            e.children = e.secured = -1;
        if (n == cap) {
            cap = cap ? cap * 2 : 64;
            rows = realloc(rows, cap * sizeof(*rows));
            if (!rows)
                abort();
        }
        rows[n++] = e;
    }
    sqlite3_finalize(s);
    *out = rows;
    return n;
}
int list_entries(Catalog *c, const char *path, Entry **out, int historical, int unsecured,
                 const char *search)
{
    return list_entries_page(c, path, out, historical, unsecured, search, 0, 0, 0, 0, NULL);
}
char *entry_details_page(Catalog *c, const Entry *e, int page)
{
    Buf b = {0};
    char *p = display_path(e->path), sz[40], mt[40], bt[40];
    human_bytes(e->size, sz);
    date_text(e->mtime, mt);
    date_text(e->btime, bt);
    badd(&b, "%s\n%s | %s (%lld B) | mode %04llo\n", p, e->kind, sz, (long long)e->size,
         (unsigned long long)e->mode & 07777);
    free(p);
    if (e->pending)
        badd(&b, "\nPENDING SCAN: observed metadata; content is not verified yet.\n");
    if (*e->hash) {
        p = display_path(e->dest);
        badd(&b, "\nDESTINATION COPY [%s]:\n%s\n", *e->state ? e->state : "missing",
             *p ? p : "not copied yet");
        free(p);
        badd(&b, "\nCONTENT OCCURRENCES (page %d; Ctrl+Alt+PgUp/PgDn):\n", page + 1);
        sqlite3_stmt *s = sql_prepare(
            c, "SELECT e.path,e.present,e.discarded,v.accepted,v.id=e.current FROM versions v JOIN "
               "entries e ON e.id=v.eid WHERE v.hash=? ORDER BY v.eid,v.id LIMIT 256 OFFSET ?");
        bind_text(s, 1, e->hash);
        sqlite3_bind_int64(s, 2, (int64_t)page * 256);
        while (sqlite3_step(s) == SQLITE_ROW) {
            p = display_path((char *)sqlite3_column_text(s, 0));
            badd(&b, "%s %s%s%s%s\n", sqlite3_column_int(s, 3) ? "[OK]" : "[ ]", p,
                 sqlite3_column_int(s, 2) ? " [rejected]" : "",
                 !sqlite3_column_int(s, 1) ? " [absent]" : "",
                 !sqlite3_column_int(s, 4) ? " [older version]" : "");
            free(p);
        }
        sqlite3_finalize(s);
    }
    if (!strcmp(e->kind, "directory")) {
        badd(&b, "\nLOCATIONS OF RETAINED FILES IN THIS SUBTREE:\n");
        sqlite3_stmt *locations = sql_prepare(
            c, "SELECT DISTINCT o.dest,o.state FROM entries x JOIN versions v ON v.eid=x.id "
               "JOIN objects o ON o.hash=v.hash WHERE v.accepted=1 AND o.dest IS NOT NULL "
               "AND (?='' OR (x.path>=?||'/' AND x.path<?||'0')) ORDER BY o.dest LIMIT 2001");
        for (int i = 1; i <= 3; i++)
            bind_text(locations, i, e->path);
        int n = 0;
        while (sqlite3_step(locations) == SQLITE_ROW) {
            if (++n > 2000) {
                badd(&b, "For more locations, open a subdirectory.\n");
                break;
            }
            char *location = display_path((char *)sqlite3_column_text(locations, 0));
            badd(&b, "[%s] %s\n", sqlite3_column_text(locations, 1), location);
            free(location);
        }
        if (!n)
            badd(&b, "No retained content; an empty directory may have retained metadata.\n");
        sqlite3_finalize(locations);
    }
    badd(&b,
         "\nModified: %s\nCreated: %s\nUID:GID %lld:%lld\nReviewed: %s | Rejected: "
         "%s\nPresent on source: %s\n",
         mt, bt, (long long)e->uid, (long long)e->gid, e->reviewed ? "yes" : "no",
         e->discarded ? "yes" : "no", e->present ? "yes" : "no");
    if (*e->error)
        badd(&b, "ERROR: %s\n", e->error);
    sqlite3_stmt *s = sql_prepare(c, "SELECT link,meta_error FROM versions WHERE id=?");
    sqlite3_bind_int64(s, 1, e->version);
    if (sqlite3_step(s) == SQLITE_ROW) {
        if (sqlite3_column_text(s, 0)) {
            p = display_path((char *)sqlite3_column_text(s, 0));
            badd(&b, "\nSymlink target: %s\n", p);
            free(p);
        }
        const char *err = (char *)sqlite3_column_text(s, 1);
        if (err && *err)
            badd(&b, "\nIncomplete metadata: %s\n", err);
    }
    sqlite3_finalize(s);
    if (*e->hash) {
        badd(&b, "\nSHA-512:\n%.64s\n%s\n", e->hash, e->hash + 64);
        s = sql_prepare(c, "SELECT verified FROM objects WHERE hash=?");
        bind_text(s, 1, e->hash);
        if (sqlite3_step(s) == SQLITE_ROW) {
            date_text(sqlite3_column_int64(s, 0), mt);
            badd(&b, "Last verification: %s\n", mt);
        }
        sqlite3_finalize(s);
    }
    badd(&b, "\nRETAINED VERSIONS AT THIS PATH (Ctrl+V in history):\n");
    s = sql_prepare(c, "SELECT v.id,v.mtime,o.dest FROM versions v LEFT JOIN objects o ON "
                       "o.hash=v.hash WHERE v.eid=? AND v.accepted=1 ORDER BY v.id DESC");
    sqlite3_bind_int64(s, 1, e->id);
    while (sqlite3_step(s) == SQLITE_ROW) {
        date_text(sqlite3_column_int64(s, 1), mt);
        p = display_path((char *)sqlite3_column_text(s, 2) ? (char *)sqlite3_column_text(s, 2)
                                                           : "metadata");
        badd(&b, "v%lld  %s  %s\n", (long long)sqlite3_column_int64(s, 0), mt, p);
        free(p);
    }
    sqlite3_finalize(s);
    return b.s;
}

char *entry_details(Catalog *c, const Entry *e)
{
    return entry_details_page(c, e, 0);
}

void catalog_summary(Catalog *c, Summary *r)
{
    memset(r, 0, sizeof(*r));
    sqlite3_stmt *s = sql_prepare(
        c,
        "SELECT COUNT(*),COALESCE(SUM(e.discarded=0 AND NOT EXISTS(SELECT 1 FROM discoveries d "
        "WHERE d.eid=e.id) AND ((o.state='verified' AND v.hash IS NOT "
        "NULL) OR (v.accepted=1 AND "
        "e.kind<>'file'))),0),COALESCE(SUM(e.discarded),0),COALESCE(SUM(e.reviewed),0),COALESCE("
        "SUM(e.error<>'' OR v.meta_error<>''),0),COALESCE(SUM(e.present=0),0),COALESCE(SUM(e.kind "
        "NOT IN ('file','directory')),0) FROM entries e LEFT JOIN versions v ON v.id=e.current "
        "LEFT JOIN objects o ON o.hash=v.hash WHERE e.path<>'' AND e.kind<>'directory'");
    if (sqlite3_step(s) == SQLITE_ROW) {
        r->paths = sqlite3_column_int64(s, 0);
        r->secured = sqlite3_column_int64(s, 1);
        r->rejected = sqlite3_column_int64(s, 2);
        r->reviewed = sqlite3_column_int64(s, 3);
        r->errors = sqlite3_column_int64(s, 4);
        r->missing = sqlite3_column_int64(s, 5);
        r->special = sqlite3_column_int64(s, 6);
    }
    sqlite3_finalize(s);
    s = sql_prepare(
        c, "SELECT COUNT(*),COALESCE(SUM(size),0),COALESCE(SUM(CASE WHEN state<>'verified' THEN "
           "size ELSE 0 END),0) FROM objects WHERE hash IN (SELECT v.hash FROM entries e JOIN "
           "versions v ON v.id=e.current WHERE e.discarded=0 AND e.present=1)");
    if (sqlite3_step(s) == SQLITE_ROW) {
        r->unique = sqlite3_column_int64(s, 0);
        r->unique_bytes = sqlite3_column_int64(s, 1);
        r->needed_bytes = sqlite3_column_int64(s, 2);
    }
    sqlite3_finalize(s);
    s = sql_prepare(c, "SELECT COUNT(*) FROM entries e LEFT JOIN versions v ON v.id=e.current "
                       "WHERE e.kind='directory' AND (e.error<>'' OR v.meta_error<>'')");
    if (sqlite3_step(s) == SQLITE_ROW)
        r->errors += sqlite3_column_int64(s, 0);
    sqlite3_finalize(s);
    struct statvfs v;
    if (!fstatvfs(c->dfd, &v)) {
        r->free_bytes = (uint64_t)v.f_bavail * v.f_frsize;
        r->total_bytes = (uint64_t)v.f_blocks * v.f_frsize;
    }
}
char *summary_text(Catalog *c)
{
    Summary s;
    catalog_summary(c, &s);
    char a[40], b[40], d[40];
    human_bytes(s.unique_bytes, a);
    human_bytes(s.needed_bytes, b);
    human_bytes(s.free_bytes, d);
    char *complete = setting(c, "scan_complete", "0");
    char *r = fmt("AUDIT / BALANCE\n"
                  "\n"
                  "Complete source traversal: %s\n"
                  "Entries (excluding directories): %llu\n"
                  "Secured: %llu\n"
                  "Intentionally rejected: %llu\n"
                  "Reviewed: %llu\n"
                  "Errors / incomplete metadata: %llu\n"
                  "Absent from source: %llu\n"
                  "Special objects / links: %llu\n"
                  "\n"
                  "Unique current contents: %llu\n"
                  "Their logical size: %s\n"
                  "Remaining logical data: %s\n"
                  "Free on destination: %s\n"
                  "\n"
                  "ZFS: compression, sparse files, reservations and overhead\n"
                  "affect actual space usage.\n"
                  "\n"
                  "Rejected entries are not migration gaps.\n"
                  "Scan errors are not rejection decisions.\n"
                  "SHA-512 audit: Ctrl+A (reread destination data).",
                  !strcmp(complete, "1") ? "yes" : "no / errors", (unsigned long long)s.paths,
                  (unsigned long long)s.secured, (unsigned long long)s.rejected,
                  (unsigned long long)s.reviewed, (unsigned long long)s.errors,
                  (unsigned long long)s.missing, (unsigned long long)s.special,
                  (unsigned long long)s.unique, a, b, d);
    free(complete);
    return r;
}

static int output_path(FILE *f, const char *key, const char *raw)
{
    char *j = json_string(raw), *b = b64(raw, strlen(raw));
    int rc = fprintf(f, "\"%s\":%s,\"%s_b64\":\"%s\"", key, j, key, b);
    free(j);
    free(b);
    return rc < 0 ? -1 : 0;
}
int export_archive(Catalog *c)
{
    progress(c, "Writing the portable JSONL index");
    int fd = openat(c->mfd, "archive.jsonl.tmp",
                    O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0)
        return fail(c, "Manifest: %s", strerror(errno));
    FILE *f = fdopen(fd, "w");
    if (!f) {
        close(fd);
        return -1;
    }
    if (sql_exec(c, "BEGIN")) {
        fclose(f);
        return -1;
    }
    fprintf(f,
            "{\"type\":\"header\",\"schema\":1,\"hash_algorithm\":\"sha512\",\"generated_ns\":\"%"
            "lld\",\"encoding\":\"path_b64 is authoritative POSIX bytes\",",
            (long long)now_ns());
    output_path(f, "source", c->source);
    fprintf(f, "}\n");
    sqlite3_stmt *s =
        sql_prepare(c, "SELECT hash,size,dest,state,verified FROM objects WHERE dest IS NOT NULL "
                       "AND hash IN(SELECT hash FROM versions WHERE accepted=1)");
    while (sqlite3_step(s) == SQLITE_ROW) {
        fprintf(f, "{\"type\":\"object\",\"sha512\":\"%s\",\"size\":%lld,",
                sqlite3_column_text(s, 0), (long long)sqlite3_column_int64(s, 1));
        output_path(f, "destination", (char *)sqlite3_column_text(s, 2));
        fprintf(f, ",\"state\":\"%s\",\"verified_ns\":\"%lld\"}\n", sqlite3_column_text(s, 3),
                (long long)sqlite3_column_int64(s, 4));
    }
    sqlite3_finalize(s);
    /* Include ancestor metadata even when only one leaf was explicitly kept. */
    s = sql_prepare(
        c, "SELECT e.path,(CASE (v.mode&61440) WHEN 32768 THEN 'file' WHEN 16384 THEN 'directory' "
           "WHEN 40960 THEN 'symlink' WHEN 4096 THEN 'fifo' WHEN 49152 THEN 'socket' WHEN 24576 "
           "THEN 'block' WHEN 8192 THEN 'character' ELSE e.kind "
           "END),v.id,v.mode,v.size,v.uid,v.gid,v.mtime,v.ctime,v.atime,v.btime,v.dev,v.ino,v.hash,"
           "v.link,v.attrs,v.meta_error,v.id=e.current FROM versions v JOIN entries e ON "
           "e.id=v.eid WHERE v.accepted=1 ORDER BY e.path,v.id");
    while (sqlite3_step(s) == SQLITE_ROW) {
        fprintf(f, "{\"type\":\"entry\",");
        output_path(f, "path", (char *)sqlite3_column_text(s, 0));
        fprintf(f,
                ",\"kind\":\"%s\",\"version\":%lld,\"mode\":%lld,\"size\":%lld,\"uid\":%lld,"
                "\"gid\":%lld,\"mtime_ns\":\"%lld\",\"ctime_ns\":\"%lld\",\"atime_ns\":\"%lld\","
                "\"btime_ns\":\"%lld\",\"dev\":\"%lld\",\"ino\":\"%lld\"",
                sqlite3_column_text(s, 1), (long long)sqlite3_column_int64(s, 2),
                (long long)sqlite3_column_int64(s, 3), (long long)sqlite3_column_int64(s, 4),
                (long long)sqlite3_column_int64(s, 5), (long long)sqlite3_column_int64(s, 6),
                (long long)sqlite3_column_int64(s, 7), (long long)sqlite3_column_int64(s, 8),
                (long long)sqlite3_column_int64(s, 9), (long long)sqlite3_column_int64(s, 10),
                (long long)sqlite3_column_int64(s, 11), (long long)sqlite3_column_int64(s, 12));
        if (sqlite3_column_text(s, 13))
            fprintf(f, ",\"sha512\":\"%s\"", sqlite3_column_text(s, 13));
        if (sqlite3_column_text(s, 14)) {
            fputc(',', f);
            output_path(f, "link", (char *)sqlite3_column_text(s, 14));
        }
        char *err = json_string((char *)sqlite3_column_text(s, 16));
        fprintf(f, ",\"xattrs\":%s,\"metadata_error\":%s,\"current_at_export\":%s}\n",
                sqlite3_column_text(s, 15), err, sqlite3_column_int(s, 17) ? "true" : "false");
        free(err);
    }
    sqlite3_finalize(s);
    /* Operational decisions remain in the session; archive history includes only placements. */
    s = sql_prepare(c, "SELECT id,time,op,src,dst,hash FROM events WHERE op IN "
                       "('copy','reuse','move','recover') ORDER BY id");
    while (sqlite3_step(s) == SQLITE_ROW) {
        fprintf(f, "{\"type\":\"event\",\"id\":%lld,\"time_ns\":\"%lld\",\"operation\":\"%s\",",
                (long long)sqlite3_column_int64(s, 0), (long long)sqlite3_column_int64(s, 1),
                sqlite3_column_text(s, 2));
        output_path(f, "from",
                    (char *)sqlite3_column_text(s, 3) ? (char *)sqlite3_column_text(s, 3) : "");
        fputc(',', f);
        output_path(f, "to",
                    (char *)sqlite3_column_text(s, 4) ? (char *)sqlite3_column_text(s, 4) : "");
        if (sqlite3_column_text(s, 5))
            fprintf(f, ",\"sha512\":\"%s\"", sqlite3_column_text(s, 5));
        fprintf(f, "}\n");
    }
    sqlite3_finalize(s);
    sql_exec(c, "COMMIT");
    int bad = ferror(f) || fflush(f) || fsync(fd);
    if (fclose(f))
        bad = 1;
    if (bad)
        return fail(c, "Manifest write failed: %s", strerror(errno));
    if (renameat(c->mfd, "archive.jsonl.tmp", c->mfd, "archive.jsonl") || fsync(c->mfd))
        return fail(c, "Manifest commit failed: %s", strerror(errno));
    return 0;
}

static char *json_value(Catalog *c, const char *line, const char *key)
{
    sqlite3_stmt *s = sql_prepare(c, "SELECT json_extract(?,?)");
    bind_text(s, 1, line);
    char *k = fmt("$.%s", key);
    bind_text(s, 2, k);
    free(k);
    char *v = sqlite3_step(s) == SQLITE_ROW ? xstrdup((char *)sqlite3_column_text(s, 0)) : NULL;
    sqlite3_finalize(s);
    return v;
}
static char *json_path(Catalog *c, const char *line, const char *key)
{
    char *k = fmt("%s_b64", key), *v = json_value(c, line, k);
    free(k);
    if (!v)
        return NULL;
    size_t n = 0;
    unsigned char *r = unb64(v, &n);
    free(v);
    if (!r || strlen((char *)r) != n) {
        free(r);
        return NULL;
    }
    return (char *)r;
}
int import_archive(Catalog *c, const char *filename)
{
    sqlite3_stmt *s = sql_prepare(c, "SELECT COUNT(*) FROM entries");
    sqlite3_step(s);
    int n = sqlite3_column_int(s, 0);
    sqlite3_finalize(s);
    if (n)
        return fail(c, "Import requires a fresh catalog (existing data are never overwritten)");
    FILE *f = fopen(filename, "r");
    if (!f)
        return fail(c, "Import: %s", strerror(errno));
    char *line = NULL;
    size_t cap = 0;
    int rc = 0, header = 0;
    sql_exec(c, "BEGIN IMMEDIATE");
    while (getline(&line, &cap, f) >= 0) {
        char *type = json_value(c, line, "type");
        if (!type) {
            rc = fail(c, "Invalid JSONL");
            break;
        }
        if (!strcmp(type, "header")) {
            char *ver = json_value(c, line, "schema");
            header = ver && atoi(ver) == 1;
            free(ver);
            if (!header) {
                free(type);
                rc = fail(c, "Unsupported manifest schema");
                break;
            }
            char *origin = json_path(c, line, "source");
            if (origin) {
                set_setting(c, "source", origin);
                free(c->source);
                c->source = origin;
            }
        } else if (!header) {
            free(type);
            rc = fail(c, "Missing manifest header");
            break;
        } else if (!strcmp(type, "object")) {
            char *hash = json_value(c, line, "sha512"), *dest = json_path(c, line, "destination"),
                 *size = json_value(c, line, "size");
            if (!hash || strlen(hash) != 128 || !dest || !valid_relative(dest, 0) ||
                descendant(dest, META_DIR)) {
                free(hash);
                free(dest);
                free(size);
                free(type);
                rc = fail(c, "Invalid object record");
                break;
            }
            s = sql_prepare(c,
                            "INSERT INTO objects(hash,size,dest,state) VALUES(?,?,?,'unchecked')");
            bind_text(s, 1, hash);
            sqlite3_bind_int64(s, 2, size ? atoll(size) : 0);
            bind_text(s, 3, dest);
            if (sqlite3_step(s) != SQLITE_DONE)
                rc = -1;
            sqlite3_finalize(s);
            free(hash);
            free(dest);
            free(size);
        } else if (!strcmp(type, "entry")) {
            char *path = json_path(c, line, "path"), *kind = json_value(c, line, "kind");
            if (!path || !valid_relative(path, 1) || !kind) {
                free(path);
                free(kind);
                free(type);
                rc = fail(c, "Invalid entry record");
                break;
            }
            char *par = parent_path(path);
            s = sql_prepare(
                c,
                "INSERT OR IGNORE INTO entries(path,parent,name,kind,present) VALUES(?,?,?,?,0)");
            bind_text(s, 1, path);
            bind_text(s, 2, *path ? par : NULL);
            bind_text(s, 3, base_name(path));
            bind_text(s, 4, kind);
            sqlite3_step(s);
            sqlite3_finalize(s);
            free(par);
            s = sql_prepare(c, "INSERT INTO "
                               "versions(eid,mode,size,uid,gid,mtime,ctime,atime,btime,dev,ino,"
                               "hash,link,attrs,meta_error,accepted) VALUES((SELECT id FROM "
                               "entries WHERE path=?),?,?,?,?,?,?,?,?,?,?,?,?,?,?,1)");
            bind_text(s, 1, path);
            const char *keys[] = {"mode",     "size",     "uid",      "gid", "mtime_ns",
                                  "ctime_ns", "atime_ns", "btime_ns", "dev", "ino"};
            for (int i = 0; i < 10; i++) {
                char *v = json_value(c, line, keys[i]);
                sqlite3_bind_int64(s, i + 2, v ? atoll(v) : 0);
                free(v);
            }
            char *hash = json_value(c, line, "sha512"), *link = json_path(c, line, "link"),
                 *attrs = json_value(c, line, "xattrs"),
                 *err = json_value(c, line, "metadata_error");
            bind_text(s, 12, hash && *hash ? hash : NULL);
            bind_text(s, 13, link);
            bind_text(s, 14, attrs && *attrs ? attrs : "[]");
            bind_text(s, 15, err ? err : "");
            if (sqlite3_step(s) != SQLITE_DONE)
                rc = -1;
            sqlite3_finalize(s);
            free(hash);
            free(link);
            free(attrs);
            free(err);
            int64_t vid = sqlite3_last_insert_rowid(c->db);
            s = sql_prepare(c, "UPDATE entries SET current=? WHERE path=?");
            sqlite3_bind_int64(s, 1, vid);
            bind_text(s, 2, path);
            sqlite3_step(s);
            sqlite3_finalize(s);
            free(path);
            free(kind);
        } else if (!strcmp(type, "event")) {
            char *op = json_value(c, line, "operation"), *from = json_path(c, line, "from"),
                 *to = json_path(c, line, "to"), *hash = json_value(c, line, "sha512"),
                 *time = json_value(c, line, "time_ns");
            s = sql_prepare(c, "INSERT INTO events(time,op,src,dst,hash) VALUES(?,?,?,?,?)");
            sqlite3_bind_int64(s, 1, time ? atoll(time) : 0);
            bind_text(s, 2, op);
            bind_text(s, 3, from);
            bind_text(s, 4, to);
            bind_text(s, 5, hash && *hash ? hash : NULL);
            if (sqlite3_step(s) != SQLITE_DONE)
                rc = -1;
            sqlite3_finalize(s);
            free(op);
            free(from);
            free(to);
            free(hash);
            free(time);
        }
        free(type);
        if (rc)
            break;
    }
    if (ferror(f))
        rc = -1;
    if (!header)
        rc = fail(c, "Empty manifest");
    free(line);
    fclose(f);
    sql_exec(c, rc ? "ROLLBACK" : "COMMIT");
    return rc;
}
