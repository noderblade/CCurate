#include "usage_internal.h"
#include <stdlib.h>
#include <string.h>

/* In directory DFS order, k occurrences inside any subtree form one contiguous
 * interval. Its k-1 adjacent pairs have LCAs inside that subtree. Assign one
 * repeated file size to each pair's LCA, then roll up: hashed - repeated is the
 * exact subtree union size. We retain only corrections for duplicate groups,
 * not a hash-membership table for every file/ancestor combination. */
static char *common_directory(const char *a, const char *b)
{
    size_t i = 0, last = 0;
    while (a[i] && b[i] && a[i] == b[i]) {
        if (a[i] == '/')
            last = i;
        i++;
    }
    if (!a[i] && (!b[i] || b[i] == '/'))
        return xstrdup(a);
    if (!b[i] && (!a[i] || a[i] == '/'))
        return xstrdup(b);
    return strndup(a, last);
}
static int group(UsageDb *db, const char *hash)
{
    Catalog *c = &db->cache;
    const char *p = db->full_build ? "next_" : "";
    char *sql = fmt("SELECT id FROM %sgroups WHERE hash=?", p);
    sqlite3_stmt *q = sql_prepare(c, sql);
    free(sql);
    if (!q)
        return -1;
    bind_text(q, 1, hash);
    int rc = sqlite3_step(q);
    int64_t gid = rc == SQLITE_ROW ? sqlite3_column_int64(q, 0) : 0;
    sqlite3_finalize(q);
    if (rc != SQLITE_ROW && rc != SQLITE_DONE)
        return -1;
    if (gid) {
        if (!db->full_build) {
            q = sql_prepare(c,
                            "INSERT OR IGNORE INTO dirty_nodes SELECT dir FROM terms WHERE gid=?");
            if (q)
                sqlite3_bind_int64(q, 1, gid);
            rc = usage_stmt_done(c, q);
            sqlite3_finalize(q);
            if (rc)
                return rc;
        }
        sql = fmt("DELETE FROM %sterms WHERE gid=?", p);
        q = sql_prepare(c, sql);
        free(sql);
        if (q)
            sqlite3_bind_int64(q, 1, gid);
        rc = usage_stmt_done(c, q);
        sqlite3_finalize(q);
        if (rc)
            return rc;
    }
    q = sql_prepare(
        c,
        "SELECT e.parent,v.size,e.discarded FROM src.versions v JOIN src.entries e "
        "ON e.current=v.id LEFT JOIN src.discoveries d ON d.eid=e.id WHERE v.hash=? AND v.size>0 "
        "AND e.kind='file' AND e.present=1 AND e.error='' AND d.eid IS NULL "
        "ORDER BY (e.parent||'/') COLLATE BINARY,e.id");
    if (!q)
        return -1;
    bind_text(q, 1, hash);
    sql = fmt("INSERT INTO %sterms(gid,dir,scope,bytes) VALUES(?,?,?,?) ON CONFLICT(gid,dir,scope) "
              "DO UPDATE SET bytes=bytes+excluded.bytes",
              p);
    sqlite3_stmt *term = sql_prepare(c, sql);
    free(sql);
    sql = fmt("SELECT id FROM %sdirs WHERE path=?", p);
    sqlite3_stmt *dir = sql_prepare(c, sql);
    free(sql);
    if (!term || !dir) {
        sqlite3_finalize(q);
        sqlite3_finalize(term);
        sqlite3_finalize(dir);
        return -1;
    }
    char *previous[2] = {NULL, NULL};
    int64_t previous_size = 0;
    int failed = 0, terms = 0;
    while (!usage_interrupted(db) && (rc = sqlite3_step(q)) == SQLITE_ROW) {
        const char *path = (const char *)sqlite3_column_text(q, 0);
        int64_t size = sqlite3_column_int64(q, 1);
        if (previous[0] && size != previous_size) {
            failed = fail(c, "Conflicting sizes in a SHA-512 group; refresh the source catalog");
            break;
        }
        previous_size = size;
        for (int scope = 0; scope < 2 && !failed; scope++) {
            if (scope && sqlite3_column_int(q, 2))
                continue;
            if (previous[scope]) {
                if (!gid) {
                    sql = fmt("INSERT INTO %sgroups(hash) VALUES(?)", p);
                    sqlite3_stmt *insert = sql_prepare(c, sql);
                    free(sql);
                    if (insert)
                        bind_text(insert, 1, hash);
                    failed = usage_stmt_done(c, insert);
                    sqlite3_finalize(insert);
                    if (failed)
                        break;
                    gid = sqlite3_last_insert_rowid(c->db);
                }
                char *common = common_directory(previous[scope], path);
                bind_text(dir, 1, common);
                free(common);
                int step = sqlite3_step(dir);
                int64_t id = step == SQLITE_ROW ? sqlite3_column_int64(dir, 0) : 0;
                sqlite3_reset(dir);
                sqlite3_clear_bindings(dir);
                if (step != SQLITE_ROW) {
                    failed = fail(c, "Missing ancestor in the usage index");
                    break;
                }
                sqlite3_bind_int64(term, 1, gid);
                sqlite3_bind_int64(term, 2, id);
                sqlite3_bind_int(term, 3, scope);
                sqlite3_bind_int64(term, 4, size);
                failed = usage_stmt_done(c, term);
                terms++;
                if (!db->full_build && !failed) {
                    sqlite3_stmt *mark =
                        sql_prepare(c, "INSERT OR IGNORE INTO dirty_nodes VALUES(?)");
                    if (mark)
                        sqlite3_bind_int64(mark, 1, id);
                    failed = usage_stmt_done(c, mark);
                    sqlite3_finalize(mark);
                }
            }
            free(previous[scope]);
            previous[scope] = xstrdup(path);
        }
        if (failed)
            break;
    }
    free(previous[0]);
    free(previous[1]);
    sqlite3_finalize(q);
    sqlite3_finalize(dir);
    sqlite3_finalize(term);
    if (failed || rc != SQLITE_DONE || usage_interrupted(db))
        return -1;
    if (gid && !terms) {
        sql = fmt("DELETE FROM %sgroups WHERE id=?", p);
        q = sql_prepare(c, sql);
        free(sql);
        if (q)
            sqlite3_bind_int64(q, 1, gid);
        rc = usage_stmt_done(c, q);
        sqlite3_finalize(q);
        if (rc)
            return rc;
    }
    return 0;
}
int usage_duplicate_build(UsageDb *db, int full)
{
    usage_phase(db, USAGE_BUILDING,
                full ? "Resolving duplicate content across directory branches"
                     : "Updating affected duplicate groups");
    Catalog *c = &db->cache;
    char *after = xstrdup("");
    int failed = 0;
    while (!usage_interrupted(db) && !failed) {
        const char *sql = full ? "SELECT hash FROM src.hash_groups WHERE hash>? AND occurrences>1 "
                                 "AND length(hash)=128 ORDER BY hash LIMIT 128"
                               : "SELECT hash FROM src.usage_dirty_hashes WHERE hash>? AND "
                                 "length(hash)=128 ORDER BY hash LIMIT 128";
        sqlite3_stmt *s = sql_prepare(c, sql);
        if (!s) {
            failed = -1;
            break;
        }
        bind_text(s, 1, after);
        char *hashes[128];
        int count = 0, rc;
        while ((rc = sqlite3_step(s)) == SQLITE_ROW && count < 128)
            hashes[count++] = xstrdup((const char *)sqlite3_column_text(s, 0));
        sqlite3_finalize(s);
        if (rc != SQLITE_DONE) {
            for (int i = 0; i < count; i++)
                free(hashes[i]);
            failed = -1;
            break;
        }
        if (!count)
            break;
        free(after);
        after = xstrdup(hashes[count - 1]);
        for (int i = 0; i < count; i++) {
            if (!failed) {
                if (full && sql_exec(c, "BEGIN"))
                    failed = -1;
                if (!failed)
                    failed = group(db, hashes[i]);
                if (full)
                    failed = sql_end(c, failed);
                pthread_mutex_lock(&db->mutex);
                db->status.done++;
                pthread_mutex_unlock(&db->mutex);
            }
            free(hashes[i]);
        }
    }
    free(after);
    return failed || usage_interrupted(db) ? -1 : 0;
}
