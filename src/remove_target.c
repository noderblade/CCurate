#include "remove_target.h"
#include "source_access.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    int64_t id;
    char *source, *target, *temp, *hash;
} Removal;

static void removal_free(Removal *r)
{
    free(r->source);
    free(r->target);
    free(r->temp);
    free(r->hash);
}

static int execute(Catalog *c, sqlite3_stmt *s)
{
    if (!s)
        return -1;
    int rc = sqlite3_step(s);
    sqlite3_finalize(s);
    return rc == SQLITE_DONE ? 0 : sql_error(c, rc, "save destination removal");
}

/* NULL detaches the copy. A non-NULL path preserves it, but requires an audit
 * after interrupted I/O. Accepted versions retain the original tree in either case. */
static int mapping(Catalog *c, const Removal *r, const char *path)
{
    sqlite3_stmt *s = sql_prepare(c, "UPDATE objects SET dest=?,state=?,verified=0 "
                                     "WHERE dest=? AND hash=?");
    if (!s)
        return -1;
    bind_text(s, 1, path);
    bind_text(s, 2, path ? "unchecked" : "missing");
    bind_text(s, 3, r->target);
    bind_text(s, 4, r->hash);
    if (execute(c, s))
        return -1;
    s = sql_prepare(c, path ? "UPDATE empty_copies SET dest=?,state='unchecked',verified=0 "
                              "WHERE dest=?"
                            : "DELETE FROM empty_copies WHERE dest=?");
    if (!s)
        return -1;
    if (path)
        bind_text(s, 1, path);
    bind_text(s, path ? 2 : 1, r->target);
    return execute(c, s);
}

static int finish(Catalog *c, const Removal *r, const char *preserved)
{
    if (sql_exec(c, "BEGIN IMMEDIATE"))
        return -1;
    int rc = mapping(c, r, preserved);
    sqlite3_stmt *s = sql_prepare(c, "UPDATE operations SET state=? WHERE id=?");
    if (s) {
        bind_text(s, 1, preserved ? "restore_export" : "remove_export");
        sqlite3_bind_int64(s, 2, r->id);
    }
    rc |= execute(c, s);
    s = sql_prepare(c, "INSERT INTO events(time,op,src,dst,hash) VALUES(?,?,?,?,?)");
    if (s) {
        sqlite3_bind_int64(s, 1, now_ns());
        bind_text(s, 2, preserved ? "recover" : "remove");
        /* Removal records go from the destination path to no destination. */
        bind_text(s, 3, r->target);
        bind_text(s, 4, preserved);
        bind_text(s, 5, r->hash);
    }
    rc |= execute(c, s);
    return sql_end(c, rc);
}

static int publish_removals(Catalog *c)
{
    sqlite3_stmt *s = sql_prepare(c, "SELECT 1 FROM operations WHERE kind='remove' AND "
                                     "state IN('remove_export','restore_export') LIMIT 1");
    if (!s)
        return -1;
    int step = sqlite3_step(s);
    sqlite3_finalize(s);
    if (step == SQLITE_DONE)
        return 0;
    if (step != SQLITE_ROW)
        return sql_error(c, step, "read pending removal export");
    /* Keep the export obligation durable until JSONL has been atomically replaced
     * and fsynced. A crash after the SQLite commit must not strand a stale index. */
    if (export_archive(c))
        return -1;
    return sql_exec(c, "UPDATE operations SET state=CASE state WHEN 'remove_export' THEN 'done' "
                       "ELSE 'interrupted' END WHERE kind='remove' AND "
                       "state IN('remove_export','restore_export')");
}

static int path_status(int parent, const char *leaf, struct stat *st)
{
    if (!fstatat(parent, leaf, st, AT_SYMLINK_NOFOLLOW))
        return 1;
    return errno == ENOENT ? 0 : -1;
}

static int recover_one(Catalog *c, const Removal *r)
{
    char *parent = parent_path(r->target), *temp_parent = parent_path(r->temp);
    int valid = valid_relative(r->target, 0) && !descendant(r->target, META_DIR) &&
                valid_relative(r->temp, 0) && !strcmp(parent, temp_parent) &&
                !strncmp(base_name(r->temp), ".ccurate-remove-", 16) && strcmp(r->target, r->temp);
    free(parent);
    free(temp_parent);
    if (!valid)
        return fail(c, "Invalid destination removal journal; no file was deleted");
    char *leaf = NULL;
    int pf = parent_beneath(c->dfd, r->target, 0, &leaf);
    if (pf < 0) {
        free(leaf);
        return fail(c, "Cannot recover destination removal at %s: %s", r->target, strerror(errno));
    }
    struct stat temp_st, dst_st;
    int temp = path_status(pf, base_name(r->temp), &temp_st);
    int dst = path_status(pf, leaf, &dst_st);
    int rc = -1;
    const char *preserved = NULL;
    if (temp < 0 || dst < 0) {
        fail(c, "Cannot inspect interrupted removal at %s: %s", r->target, strerror(errno));
        goto out;
    }
    if (temp) {
        if (!S_ISREG(temp_st.st_mode)) {
            fail(c, "Unexpected staged file at %s; preserved without deletion", r->temp);
            goto out;
        }
        /* A crash invalidates the source verification. Restore bytes, never
         * continue unlinking. A new file at the old name must not be overwritten. */
        if (!dst && !renameat2(pf, base_name(r->temp), pf, leaf, RENAME_NOREPLACE))
            preserved = r->target;
        else
            preserved = r->temp;
    } else if (dst)
        preserved = r->target;
    if (fsync(pf)) {
        fail(c, "Cannot sync recovered removal at %s: %s", r->target, strerror(errno));
        goto out;
    }
    rc = finish(c, r, preserved);
    if (!rc)
        activity_log(c, preserved ? LOG_WARNING : LOG_INFO, "remove.recover", r->source,
                     preserved ? preserved : r->target,
                     preserved ? "Interrupted removal: copy preserved here; audit before reuse."
                               : "Removal completed before interruption; index repaired.");
out:
    close(pf);
    free(leaf);
    return rc;
}

int recover_target_removals(Catalog *c)
{
    sqlite3_stmt *s = sql_prepare(c, "SELECT id,src,dst,temp,hash FROM operations "
                                     "WHERE kind='remove' AND state='pending' ORDER BY id");
    if (!s)
        return -1;
    Removal *rows = NULL;
    int count = 0, step;
    while ((step = sqlite3_step(s)) == SQLITE_ROW) {
        rows = realloc(rows, (count + 1) * sizeof(*rows));
        if (!rows)
            abort();
        rows[count++] =
            (Removal){sqlite3_column_int64(s, 0), xstrdup((const char *)sqlite3_column_text(s, 1)),
                      xstrdup((const char *)sqlite3_column_text(s, 2)),
                      xstrdup((const char *)sqlite3_column_text(s, 3)),
                      xstrdup((const char *)sqlite3_column_text(s, 4))};
    }
    sqlite3_finalize(s);
    int rc = step == SQLITE_DONE ? 0 : sql_error(c, step, "read destination removal journal");
    for (int i = 0; i < count; i++) {
        if (!rc)
            rc = recover_one(c, rows + i);
        removal_free(rows + i);
    }
    free(rows);
    if (!rc)
        rc = publish_removals(c);
    return rc;
}

static int begin_removal(Catalog *c, Removal *r)
{
    if (sql_exec(c, "BEGIN IMMEDIATE"))
        return -1;
    sqlite3_stmt *s = sql_prepare(c, "INSERT INTO operations(kind,src,dst,hash) "
                                     "VALUES('remove',?,?,?)");
    if (s) {
        bind_text(s, 1, r->source);
        bind_text(s, 2, r->target);
        bind_text(s, 3, r->hash);
    }
    int rc = execute(c, s);
    r->id = sqlite3_last_insert_rowid(c->db);
    char *parent = parent_path(r->target);
    char *name = fmt(".ccurate-remove-%lld-%lld.pending", (long long)r->id, (long long)now_ns());
    r->temp = join_path(parent, name);
    free(name);
    free(parent);
    s = sql_prepare(c, "UPDATE operations SET temp=? WHERE id=?");
    if (s) {
        bind_text(s, 1, r->temp);
        sqlite3_bind_int64(s, 2, r->id);
    }
    rc |= execute(c, s);
    rc |= mapping(c, r, r->target);
    return sql_end(c, rc);
}

static int same_named_file(Catalog *c, int root, const char *path, int fd,
                           const struct stat *expected)
{
    struct stat actual, named;
    int check = open_beneath(root, path, O_RDONLY | O_NONBLOCK, 0);
    int same = check >= 0 && !fstat(fd, &actual) && !fstat(check, &named) &&
               stat_same(expected, &actual) && stat_same(expected, &named);
    if (check >= 0)
        close(check);
    if (!same)
        return fail(c, "File changed during removal verification: %s", path);
    return 0;
}

static int source_copy(Catalog *c, Removal *r, struct stat *stable)
{
    int empty = !strcmp(r->hash, EMPTY_SHA512);
    sqlite3_stmt *s = sql_prepare(
        c, empty ? "SELECT e.path FROM empty_copies z JOIN versions v ON v.id=z.version "
                   "JOIN entries e ON e.id=v.eid WHERE z.dest=?"
                 : "SELECT e.path FROM versions v JOIN entries e ON e.id=v.eid WHERE v.hash=?");
    if (!s)
        return -1;
    bind_text(s, 1, empty ? r->target : r->hash);
    source_writers_reset(c);
    int fd = -1, step;
    while ((step = sqlite3_step(s)) == SQLITE_ROW && !atomic_load(&c->cancel)) {
        const char *path = (const char *)sqlite3_column_text(s, 0);
        char *absolute = join_path(c->source, path);
        int in_destination = descendant(absolute, c->destination);
        free(absolute);
        if (in_destination)
            continue;
        fd = open_beneath(c->sfd, path, O_RDONLY | O_NONBLOCK, 0);
        struct stat st;
        char hash[129];
        progress(c, "Removal: verifying source %s", path);
        if (fd >= 0 && !fstat(fd, &st) && S_ISREG(st.st_mode) && !source_writer_busy(c, &st, fd) &&
            !hash_fd(c, fd, hash, stable) && !strcmp(hash, r->hash) &&
            !same_named_file(c, c->sfd, path, fd, stable)) {
            r->source = xstrdup(path);
            break;
        }
        if (fd >= 0)
            close(fd);
        fd = -1;
    }
    sqlite3_finalize(s);
    if (fd < 0) {
        if (atomic_load(&c->cancel))
            fail(c, "Removal cancelled; destination copy kept");
        else if (step != SQLITE_DONE)
            sql_error(c, step, "find a source copy before removal");
        else
            fail(c, "Removal refused: no stable source copy has the same SHA-512. "
                    "The source may be missing, changed or open for writing. "
                    "Keep this copy or use F6 to move it.");
    } else
        c->error[0] = 0;
    return fd;
}

int remove_target_copy(Catalog *c, const char *path)
{
    if (c->readonly || c->sfd < 0)
        return fail(c, "Removal requires the live source for SHA-512 verification");
    if (!valid_relative(path, 0) || descendant(path, META_DIR))
        return fail(c, "Removal requires a file inside the destination, outside " META_DIR);
    if (atomic_load(&c->cancel))
        return fail(c, "Removal cancelled; destination copy kept");
    Removal r = {.target = xstrdup(path)};
    int rc = -1, fd = -1, src = -1, pf = -1, pending = 0, staged = 0, unlinked = 0;
    char *leaf = NULL;
    sqlite3_stmt *s =
        sql_prepare(c, "SELECT hash FROM objects WHERE dest=? AND size>0 UNION ALL "
                       "SELECT '" EMPTY_SHA512 "' FROM empty_copies WHERE dest=? LIMIT 1");
    if (!s)
        goto out;
    bind_text(s, 1, path);
    bind_text(s, 2, path);
    int step = sqlite3_step(s);
    if (step == SQLITE_ROW)
        r.hash = xstrdup((const char *)sqlite3_column_text(s, 0));
    sqlite3_finalize(s);
    if (!r.hash) {
        fail(c,
             "Select one indexed destination file. Folders and unindexed files cannot be removed.");
        goto out;
    }
    fd = open_beneath(c->dfd, path, O_RDONLY | O_NONBLOCK, 0);
    struct stat target, source, after;
    char hash[129];
    progress(c, "Removal: verifying destination %s", path);
    if (fd < 0 || fstat(fd, &target) || !S_ISREG(target.st_mode)) {
        fail(c, "Cannot open an indexed regular destination file: %s", path);
        goto out;
    }
    if (hash_fd(c, fd, hash, &target))
        goto out;
    if (strcmp(hash, r.hash)) {
        fail(c, "Removal refused: destination content changed; run an audit and keep this file");
        goto out;
    }
    src = source_copy(c, &r, &source);
    if (src < 0 || same_named_file(c, c->dfd, path, fd, &target))
        goto out;
    pf = parent_beneath(c->dfd, path, 0, &leaf);
    if (pf < 0) {
        fail(c, "Cannot open destination parent: %s", strerror(errno));
        goto out;
    }
    if (begin_removal(c, &r))
        goto out;
    pending = 1;
    /* Stage beside the file, so this also works inside nested ZFS datasets.
     * The durable journal always precedes the rename. Never overwrite a name. */
    if (renameat2(pf, leaf, pf, base_name(r.temp), RENAME_NOREPLACE)) {
        fail(c, "Cannot stage destination removal: %s", strerror(errno));
        goto out;
    }
    staged = 1;
    if (fsync(pf)) {
        fail(c, "Cannot sync staged destination removal: %s", strerror(errno));
        goto out;
    }
    /* Verify again after rename: never unlink a replacement that raced the first
     * check. Rename can change ctime, so comparing the old stat alone is insufficient. */
    progress(c, "Removal: checking staged destination %s", path);
    if (hash_fd(c, fd, hash, &after))
        goto out;
    if (strcmp(hash, r.hash)) {
        fail(c, "Removal stopped: destination changed during verification");
        goto out;
    }
    if (same_named_file(c, c->dfd, r.temp, fd, &after))
        goto out;
    source_writers_reset(c);
    if (atomic_load(&c->cancel) || same_named_file(c, c->sfd, r.source, src, &source) ||
        source_writer_busy(c, &source, src)) {
        fail(c, "Removal stopped: cancelled or source changed/open for writing; destination kept");
        goto out;
    }
    if (same_named_file(c, c->dfd, r.temp, fd, &after))
        goto out;
    if (unlinkat(pf, base_name(r.temp), 0)) {
        fail(c, "Cannot remove destination copy: %s", strerror(errno));
        goto out;
    }
    unlinked = 1;
    if (fsync(pf)) {
        fail(c, "Cannot sync removal; journal retained for recovery: %s", strerror(errno));
        goto out;
    }
    if (finish(c, &r, NULL))
        goto out;
    pending = 0;
    activity_log(
        c, LOG_INFO, "remove.complete", r.source, path,
        "Destination copy removed after SHA-512 verification; source and history retained.");
    rc = publish_removals(c);
out:
    if (fd >= 0)
        close(fd);
    if (src >= 0)
        close(src);
    if (pf >= 0)
        close(pf);
    free(leaf);
    if (pending && !unlinked) {
        char original[sizeof(c->error)];
        snprintf(original, sizeof(original), "%s", c->error);
        /* A failed rename must not adopt an unrelated file at the staging name. */
        int recovered = staged ? recover_one(c, &r) : finish(c, &r, r.target);
        if (!recovered)
            recovered = publish_removals(c);
        if (!recovered)
            snprintf(c->error, sizeof(c->error), "%.980s; copy preserved (audit before reuse)",
                     original);
        else
            activity_log(c, LOG_ERROR, "remove.recovery", r.source, r.temp,
                         "Pending removal requires recovery on restart: %s", c->error);
    }
    if (rc)
        activity_log(c, LOG_WARNING, "remove.incomplete", r.source, path, "%s", c->error);
    removal_free(&r);
    return rc;
}
