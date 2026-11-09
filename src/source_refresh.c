#include "source_refresh.h"
#include "source_access.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <linux/magic.h>
#include <sys/vfs.h>
#endif
#include <unistd.h>

int source_needs_initial_scan(Catalog *c)
{
    sqlite3_stmt *s = sql_prepare(c, "SELECT NOT EXISTS(SELECT 1 FROM entries LIMIT 1) AND "
                                     "NOT EXISTS(SELECT 1 FROM settings WHERE key='scan_started')");
    int initial = s && sqlite3_step(s) == SQLITE_ROW && sqlite3_column_int(s, 0);
    sqlite3_finalize(s);
    return initial;
}

static int cancelled(void *data)
{
    Catalog *c = data;
    return atomic_load(&c->cancel);
}

static int prepare(Catalog *c)
{
    atomic_store(&c->refresh_changed, 0);
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
    source_writers_reset(c);
    /* On supported sessions the duplicate index is already prepared by the
     * initial scan. No tree-wide query is needed for folder refresh. */
    return sql_exec(c, "CREATE TEMP TABLE IF NOT EXISTS refresh_queue("
                       "path TEXT PRIMARY KEY,bytes INTEGER,hashing INTEGER);"
                       "DELETE FROM refresh_queue;"
                       "CREATE TEMP TABLE IF NOT EXISTS refresh_seen(path TEXT PRIMARY KEY);"
                       "DELETE FROM refresh_seen;");
}

static int finish(Catalog *c, const char *path, int rc)
{
    if (!sqlite3_get_autocommit(c->db))
        sql_rollback(c);
    sqlite3_progress_handler(c->db, 0, NULL, NULL);
    if (atomic_load(&c->cancel))
        rc = fail(c, "Folder refresh cancelled; completed observations are saved");
    else if (rc && !*c->error)
        fail(c, "Some entries remain unscanned; see Activity for individual reasons");
    atomic_store(&c->scan.finished, monotonic_ns());
    atomic_store(&c->scan.phase, rc ? SCAN_STOPPED : SCAN_DONE);
    activity_log(c, rc ? LOG_WARNING : LOG_DEBUG, "refresh.finish", path, NULL,
                 "Checked %llu entries; hashed %llu bytes; errors=%llu; %s",
                 (unsigned long long)atomic_load(&c->scan.found),
                 (unsigned long long)atomic_load(&c->bytes),
                 (unsigned long long)atomic_load(&c->scan.errors), rc ? c->error : "complete");
    progress(c, rc ? "Folder refresh incomplete; see Activity" : "Folder checked: /%s", path);
    return rc;
}

static int verify_queue(Catalog *c, int64_t generation)
{
    int result = 0;
    atomic_store(&c->scan.phase, SCAN_HASH);
    atomic_store(&c->scan.phase_started, monotonic_ns());
    while (!atomic_load(&c->cancel)) {
        sqlite3_stmt *s = sql_prepare(c, "SELECT path,bytes,hashing FROM refresh_queue "
                                         "ORDER BY path LIMIT 1");
        if (!s)
            return -1;
        int step = sqlite3_step(s);
        if (step != SQLITE_ROW) {
            sqlite3_finalize(s);
            return step == SQLITE_DONE ? result : sql_error(c, step, "read folder refresh queue");
        }
        char *path = xstrdup((const char *)sqlite3_column_text(s, 0));
        int64_t bytes = sqlite3_column_int64(s, 1);
        int hashing = sqlite3_column_int(s, 2);
        sqlite3_finalize(s);
        atomic_store(&c->scan.current_plan, bytes);
        atomic_store(&c->scan.current_bytes, 0);
        progress(c, "Folder verification: %s", path);
        if (sql_exec(c, "BEGIN IMMEDIATE")) {
            free(path);
            return -1;
        }
        int rc = scan_one(c, path, 0, generation);
        /* An interrupted read must not replace the old current version with
         * a failed version. The already committed observation remains pending. */
        if (atomic_load(&c->cancel)) {
            sql_rollback(c);
            free(path);
            return -1;
        }
        if (sql_end(c, 0)) {
            free(path);
            return -1;
        }
        if (rc) {
            result = -1;
            atomic_fetch_add(&c->scan.errors, 1);
        }
        atomic_fetch_add(&c->scan.done, 1);
        atomic_fetch_add(&c->scan.resolved_bytes, bytes);
        atomic_fetch_add(&c->scan.hashed_files, hashing);
        atomic_store(&c->scan.current_plan, 0);
        atomic_store(&c->scan.current_bytes, 0);
        s = sql_prepare(c, "DELETE FROM refresh_queue WHERE path=?");
        bind_text(s, 1, path);
        step = sqlite3_step(s);
        sqlite3_finalize(s);
        free(path);
        if (step != SQLITE_DONE)
            return sql_error(c, step, "finish folder entry");
    }
    return -1;
}

/* Only proven absences in a successfully enumerated directory are removed
 * from the LIVE inventory. Traverse their catalog descendants through the
 * parent index; never touch retained versions or destination objects. */
static int mark_absent(Catalog *c, const char *path)
{
    sqlite3_stmt *s = sql_prepare(
        c, "WITH RECURSIVE gone(id,path) AS ("
           "SELECT id,path FROM entries WHERE parent=? AND present=1 AND "
           "NOT EXISTS(SELECT 1 FROM refresh_seen s WHERE s.path=entries.path) UNION "
           "SELECT child.id,child.path FROM entries child JOIN entries parent ON "
           "parent.path=child.parent JOIN discoveries d ON d.eid=parent.id WHERE "
           "parent.parent=? AND (d.mode&61440)<>16384 UNION "
           "SELECT e.id,e.path FROM entries e JOIN gone g ON e.parent=g.path) "
           "UPDATE entries SET present=0 WHERE present=1 AND id IN (SELECT id FROM gone)");
    if (!s)
        return -1;
    bind_text(s, 1, path);
    bind_text(s, 2, path);
    int step = sqlite3_step(s);
    if (step == SQLITE_DONE && sqlite3_changes(c->db))
        atomic_store(&c->refresh_changed, 1);
    sqlite3_finalize(s);
    return step == SQLITE_DONE ? 0 : sql_error(c, step, "update folder absences");
}

int refresh_directory(Catalog *c, const char *path)
{
    if (c->readonly || c->sfd < 0 || !valid_relative(path, 1))
        return fail(c, "Folder refresh requires an available source and a relative path");
    if (prepare(c))
        return finish(c, path, -1);
    activity_log(c, LOG_DEBUG, "refresh.start", path, NULL, "Checking direct children only");
    progress(c, "Checking folder metadata: /%s", path);
    int fd = open_beneath(c->sfd, path, O_RDONLY | O_DIRECTORY, 0);
    struct stat before, target;
    if (fd < 0)
        return finish(c, path, fail(c, "Cannot list /%s: %s", path, strerror(errno)));
    int unsafe = fstat(fd, &before) || fstat(c->dfd, &target);
    if (!unsafe)
        unsafe = before.st_dev == target.st_dev && before.st_ino == target.st_ino;
#ifndef _WIN32
    struct statfs fs;
    if (!fstatfs(fd, &fs) && (fs.f_type == PROC_SUPER_MAGIC || fs.f_type == SYSFS_MAGIC ||
                              fs.f_type == DEBUGFS_MAGIC || fs.f_type == TRACEFS_MAGIC ||
                              fs.f_type == CGROUP_SUPER_MAGIC || fs.f_type == CGROUP2_SUPER_MAGIC))
        unsafe = 1;
#endif
    if (unsafe) {
        close(fd);
        return finish(c, path, fail(c, "Destination or virtual kernel directory: not traversed"));
    }
    DIR *dir = fdopendir(fd);
    if (!dir) {
        close(fd);
        return finish(c, path, fail(c, "Cannot read directory: %s", strerror(errno)));
    }
    int64_t generation = now_ns();
    int rc = sql_exec(c, "BEGIN IMMEDIATE"), read_error = 0;
    if (!rc)
        rc = catalog_observe(c, path, generation);
    unsigned batch = 0;
    while (!rc && !atomic_load(&c->cancel)) {
        errno = 0;
        struct dirent *entry = readdir(dir);
        if (!entry) {
            read_error = errno;
            break;
        }
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        char *child = join_path(path, entry->d_name);
        sqlite3_stmt *s = sql_prepare(c, "INSERT OR IGNORE INTO refresh_seen VALUES(?)");
        bind_text(s, 1, child);
        int step = sqlite3_step(s);
        sqlite3_finalize(s);
        rc = step == SQLITE_DONE ? catalog_observe(c, child, generation)
                                 : sql_error(c, step, "record directory listing");
        free(child);
        if (!rc && ++batch % 128 == 0) {
            rc = sql_end(c, 0);
            if (!rc)
                rc = sql_exec(c, "BEGIN IMMEDIATE");
        }
    }
    struct stat after, named;
    int probe = open_beneath(c->sfd, path, O_RDONLY | O_DIRECTORY, 0);
    int stable = !fstat(fd, &after) && stat_same(&before, &after) && probe >= 0 &&
                 !fstat(probe, &named) && stat_same(&after, &named);
    if (probe >= 0)
        close(probe);
    closedir(dir);
    /* Commit useful observations even when enumeration is cancelled. */
    if (sql_end(c, rc))
        return finish(c, path, -1);
    if (atomic_load(&c->cancel))
        return finish(c, path, -1);
    if (read_error || !stable)
        return finish(c, path,
                      fail(c,
                           read_error ? "Directory listing failed: %s"
                                      : "Directory changed while listing: %s",
                           read_error ? strerror(read_error) : path));
    if (sql_exec(c, "BEGIN IMMEDIATE"))
        return finish(c, path, -1);
    sqlite3_progress_handler(c->db, 1000, cancelled, c);
    rc = mark_absent(c, path);
    sqlite3_progress_handler(c->db, 0, NULL, NULL);
    if (sql_end(c, rc))
        return finish(c, path, -1);
    rc = verify_queue(c, generation);
    return finish(c, path, rc);
}

int refresh_copy_source(Catalog *c, const char *path)
{
    if (c->readonly || c->sfd < 0 || !valid_relative(path, 1))
        return fail(c, "Copy refresh requires an available source and a relative path");
    /* Copying a whole directory explicitly expands its scope, so new files in
     * unvisited descendants are included too. The work queue is on disk. */
    int fd = open_beneath(c->sfd, path, O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0) {
        if (prepare(c) || sql_exec(c, "BEGIN IMMEDIATE"))
            return -1;
        int rc = catalog_observe(c, path, now_ns());
        if (sql_end(c, rc))
            return -1;
        return finish(c, path, verify_queue(c, now_ns()));
    }
    close(fd);
    if (sql_exec(c, "CREATE TEMP TABLE IF NOT EXISTS refresh_tree(path TEXT PRIMARY KEY);"
                    "DELETE FROM refresh_tree;"))
        return -1;
    sqlite3_stmt *s = sql_prepare(c, "INSERT INTO refresh_tree VALUES(?)");
    bind_text(s, 1, path);
    int step = sqlite3_step(s);
    sqlite3_finalize(s);
    if (step != SQLITE_DONE)
        return sql_error(c, step, "queue copy refresh");
    while (!atomic_load(&c->cancel)) {
        s = sql_prepare(c, "SELECT path FROM refresh_tree ORDER BY path LIMIT 1");
        step = sqlite3_step(s);
        char *next = step == SQLITE_ROW ? xstrdup((const char *)sqlite3_column_text(s, 0)) : NULL;
        sqlite3_finalize(s);
        if (!next)
            return step == SQLITE_DONE ? 0 : sql_error(c, step, "read copy refresh queue");
        /* Match the full scanner's bound on mount cycles / pathological nesting. */
        unsigned depth = 0;
        for (const char *p = next; *p; p++)
            depth += *p == '/';
        int rc =
            depth > 512 ? fail(c, "Directory nesting limit reached") : refresh_directory(c, next);
        if (!rc) {
            s = sql_prepare(c, "INSERT OR IGNORE INTO refresh_tree SELECT path FROM entries "
                               "WHERE parent=? AND present=1 AND discarded=0 AND kind='directory'");
            bind_text(s, 1, next);
            step = sqlite3_step(s);
            sqlite3_finalize(s);
            if (step != SQLITE_DONE)
                rc = sql_error(c, step, "queue descendant refresh");
        }
        s = sql_prepare(c, "DELETE FROM refresh_tree WHERE path=?");
        bind_text(s, 1, next);
        step = sqlite3_step(s);
        sqlite3_finalize(s);
        free(next);
        if (rc)
            return rc;
        if (step != SQLITE_DONE)
            return sql_error(c, step, "finish copy refresh");
    }
    return fail(c, "Copy refresh cancelled");
}
