#include "usage_internal.h"
#include <stdlib.h>
#include <string.h>

void usage_row_free(UsageRow *r)
{
    free(r->path);
    free(r->name);
    free(r->kind);
    memset(r, 0, sizeof(*r));
}
void usage_result_free(UsageResult *r)
{
    if (!r)
        return;
    for (int i = 0; i < r->count; i++)
        usage_row_free(r->rows + i);
    for (int i = 0; i < r->node_count; i++)
        usage_row_free(&r->nodes[i].row);
    free(r->rows);
    free(r->path);
    free(r->details);
    free(r->error);
    free(r);
}
void usage_request_clear(UsageRequest *r)
{
    free(r->path);
    free(r->selection);
    for (int i = 0; i < r->expanded_count; i++)
        free(r->expanded[i]);
    memset(r, 0, sizeof(*r));
}
void usage_request_copy(UsageRequest *to, const UsageRequest *from)
{
    *to = *from;
    to->path = xstrdup(from->path);
    to->selection = xstrdup(from->selection);
    if (to->expanded_count < 0)
        to->expanded_count = 0;
    if (to->expanded_count > USAGE_EXPAND_LIMIT)
        to->expanded_count = USAGE_EXPAND_LIMIT;
    for (int i = 0; i < to->expanded_count; i++)
        to->expanded[i] = xstrdup(from->expanded[i]);
    if (to->depth < 1)
        to->depth = 1;
    if (to->depth > 5)
        to->depth = 5;
    if (to->sort < 0 || to->sort >= USAGE_COLUMNS)
        to->sort = USAGE_BYTES;
    if (to->page < 0)
        to->page = 0;
    if (to->occurrence_page < 0)
        to->occurrence_page = 0;
    to->keep = !!to->keep;
}
static const char *row_sql =
    "SELECT e.id,e.path,e.name,e.kind,e.discarded,"
    "CASE WHEN e.kind='directory' THEN COALESCE(t.bytes,0) WHEN e.kind='file' THEN "
    "MAX(0,COALESCE(d.size,v.size,0)) ELSE 0 END AS bytes,"
    "CASE WHEN e.kind='directory' THEN COALESCE(t.hashed,0) WHEN e.kind='file' AND e.error='' AND "
    "d.eid IS NULL AND v.size>0 AND length(v.hash)=128 THEN v.size ELSE 0 END AS hashed,"
    "CASE WHEN e.kind='directory' THEN COALESCE(t.repeated,0) ELSE 0 END AS repeated,"
    "CASE WHEN e.kind='directory' THEN COALESCE(t.files,0) ELSE e.kind='file' END AS files,"
    "CASE WHEN e.kind='directory' THEN COALESCE(t.unknown_bytes,0) WHEN e.kind='file' AND NOT "
    "(v.id IS NOT NULL AND e.error='' AND d.eid IS NULL AND (v.size=0 OR "
    "COALESCE(length(v.hash),0)=128)) THEN MAX(0,COALESCE(d.size,v.size,0)) ELSE 0 END AS "
    "unknown_bytes,"
    "CASE WHEN e.kind='directory' THEN COALESCE(t.unknown_files,0) ELSE e.kind='file' AND NOT "
    "(v.id IS NOT NULL AND e.error='' AND d.eid IS NULL AND (v.size=0 OR "
    "COALESCE(length(v.hash),0)=128)) END AS unknown_files,"
    "CASE WHEN e.kind='directory' THEN COALESCE(t.entries,0) ELSE 1 END AS entries,"
    "CASE WHEN e.kind='directory' THEN COALESCE(t.errors,0) ELSE e.error<>'' END AS errors "
    "FROM src.entries e LEFT JOIN src.versions v ON v.id=e.current LEFT JOIN src.discoveries d ON "
    "d.eid=e.id "
    "LEFT JOIN totals t ON t.dir=e.id AND t.scope=?1 WHERE e.parent=?2 AND e.path<>'' AND "
    "e.present=1 "
    "AND (?1=0 OR e.kind='directory' OR e.discarded=0) ";
static UsageRow read_row(sqlite3_stmt *s, int depth)
{
    UsageRow row = {0};
    row.id = sqlite3_column_int64(s, 0);
    row.path = xstrdup((const char *)sqlite3_column_text(s, 1));
    row.name = xstrdup((const char *)sqlite3_column_text(s, 2));
    row.kind = xstrdup((const char *)sqlite3_column_text(s, 3));
    row.discarded = sqlite3_column_int(s, 4);
    row.depth = depth;
    usage_totals_read(s, 5, &row.totals);
    return row;
}
static int64_t children_count(UsageDb *db, const char *path, int keep)
{
    sqlite3_stmt *s = sql_prepare(
        &db->cache,
        "SELECT t.entries FROM direct t JOIN dirs d ON d.id=t.dir WHERE d.path=? AND t.scope=?");
    if (!s)
        return -1;
    bind_text(s, 1, path);
    sqlite3_bind_int(s, 2, keep);
    int rc = sqlite3_step(s);
    int64_t count = rc == SQLITE_ROW ? sqlite3_column_int64(s, 0) : 0;
    sqlite3_finalize(s);
    return rc == SQLITE_ROW || rc == SQLITE_DONE ? count : -1;
}
static int list(UsageDb *db, const UsageRequest *r, const char *path, int64_t offset, int limit,
                int sort, int descending, UsageRow *rows, int depth)
{
    const char *keys[] = {"name COLLATE BINARY", "bytes",    "bytes",
                          "(hashed-repeated)",   "repeated", "files",
                          "unknown_bytes"};
    char *sql = fmt("SELECT * FROM (%s) ORDER BY %s %s,name COLLATE BINARY,id LIMIT ?3 OFFSET ?4",
                    row_sql, keys[sort], descending ? "DESC" : "ASC");
    sqlite3_stmt *s = sql_prepare(&db->cache, sql);
    free(sql);
    if (!s)
        return -1;
    sqlite3_bind_int(s, 1, r->keep);
    bind_text(s, 2, path);
    sqlite3_bind_int(s, 3, limit);
    sqlite3_bind_int64(s, 4, offset);
    int n = 0, rc;
    while ((rc = sqlite3_step(s)) == SQLITE_ROW && n < limit)
        rows[n++] = read_row(s, depth);
    sqlite3_finalize(s);
    if (rc != SQLITE_DONE) {
        for (int i = 0; i < n; i++)
            usage_row_free(rows + i);
        return -1;
    }
    return n;
}
static int expanded(const UsageRequest *r, const char *path)
{
    for (int i = 0; i < r->expanded_count; i++)
        if (!strcmp(r->expanded[i], path))
            return 1;
    return 0;
}
static int tree(UsageDb *db, const UsageRequest *r, UsageResult *out, const char *path, int page,
                int depth)
{
    int limit = depth ? 100 : USAGE_PAGE;
    if (limit > USAGE_TREE_LIMIT - out->count)
        limit = USAGE_TREE_LIMIT - out->count;
    if (limit <= 0)
        return 0;
    UsageRow *rows = xmalloc((size_t)limit * sizeof(*rows));
    int n =
        list(db, r, path, (int64_t)page * USAGE_PAGE, limit, r->sort, r->descending, rows, depth);
    if (n < 0) {
        free(rows);
        return -1;
    }
    int failed = 0;
    for (int i = 0; i < n; i++) {
        if (out->count >= USAGE_TREE_LIMIT) {
            usage_row_free(rows + i);
            continue;
        }
        UsageRow *added = out->rows + out->count++;
        *added = rows[i];
        memset(rows + i, 0, sizeof(*rows));
        /* Keep enough space for every sibling already fetched. */
        if (!failed && !strcmp(added->kind, "directory") && expanded(r, added->path) &&
            out->count + n - i + 102 < USAGE_TREE_LIMIT && depth < USAGE_EXPAND_LIMIT)
            failed = tree(db, r, out, added->path, 0, depth + 1);
    }
    free(rows);
    if (depth && n == limit && out->count < USAGE_TREE_LIMIT &&
        children_count(db, path, r->keep) > n) {
        UsageRow *more = out->rows + out->count++;
        more->path = xstrdup(path);
        more->name = xstrdup("More entries — open this folder");
        more->kind = xstrdup("directory");
        more->depth = depth;
        more->more = 1;
    }
    return failed;
}
static int chart(UsageDb *db, const UsageRequest *r, UsageResult *out, const char *path, int parent,
                 int depth, int64_t bytes)
{
    if (depth >= r->depth || out->node_count + 13 > USAGE_CHART_LIMIT || bytes <= 0)
        return 0;
    UsageRow rows[12] = {0};
    int n = list(db, r, path, 0, 12, USAGE_BYTES, 1, rows, depth);
    if (n < 0)
        return -1;
    int indices[12], count = 0;
    int64_t shown = 0;
    for (int i = 0; i < n; i++) {
        if (rows[i].totals.bytes <= 0) {
            usage_row_free(rows + i);
            continue;
        }
        int at = out->node_count++;
        indices[count++] = at;
        UsageNode *node = out->nodes + at;
        node->row = rows[i];
        node->parent = parent;
        shown += rows[i].totals.bytes;
    }
    if (bytes > shown) {
        UsageNode *other = out->nodes + out->node_count++;
        other->parent = parent;
        other->other = 1;
        other->row.path = xstrdup(path);
        other->row.name = xstrdup("Other entries");
        other->row.kind = xstrdup("directory");
        other->row.depth = depth;
        other->row.totals.bytes = bytes - shown;
    }
    for (int i = 0; i < count; i++) {
        UsageNode *node = out->nodes + indices[i];
        if (!strcmp(node->row.kind, "directory") &&
            chart(db, r, out, node->row.path, indices[i], depth + 1, node->row.totals.bytes))
            return -1;
    }
    return 0;
}
static char *details(UsageDb *db, const UsageRequest *r, const UsageResult *out)
{
    const char *path = r->selection && *r->selection ? r->selection : out->path;
    Buf b = {0};
    char *display = display_path(path);
    badd(&b, "/%s\n", display);
    free(display);
    sqlite3_stmt *s = sql_prepare(
        &db->cache,
        "SELECT e.kind,v.hash,e.error,CASE WHEN v.size=0 THEN ec.dest ELSE o.dest END,"
        "CASE WHEN v.size=0 THEN ec.state ELSE o.state END,"
        "d.eid IS NOT NULL,v.size,v.id FROM src.entries e LEFT JOIN src.versions v ON "
        "v.id=e.current "
        "LEFT JOIN src.discoveries d ON d.eid=e.id LEFT JOIN src.objects o ON o.hash=v.hash "
        "LEFT JOIN src.empty_copies ec ON ec.version=v.id WHERE "
        "e.path=?");
    if (!s)
        return b.s;
    bind_text(s, 1, path);
    int rc = sqlite3_step(s);
    char *kind = xstrdup(rc == SQLITE_ROW ? (const char *)sqlite3_column_text(s, 0) : "directory");
    char *hash = xstrdup(rc == SQLITE_ROW ? (const char *)sqlite3_column_text(s, 1) : "");
    char *error = xstrdup(rc == SQLITE_ROW ? (const char *)sqlite3_column_text(s, 2) : "");
    char *dest = xstrdup(rc == SQLITE_ROW ? (const char *)sqlite3_column_text(s, 3) : "");
    char *state = xstrdup(rc == SQLITE_ROW ? (const char *)sqlite3_column_text(s, 4) : "");
    int pending = rc == SQLITE_ROW ? sqlite3_column_int(s, 5) : 0;
    int64_t size = rc == SQLITE_ROW ? sqlite3_column_int64(s, 6) : 0;
    int verified_version = rc == SQLITE_ROW && sqlite3_column_type(s, 7) != SQLITE_NULL;
    sqlite3_finalize(s);
    if (!strcmp(kind, "directory")) {
        s = sql_prepare(
            &db->cache,
            "SELECT "
            "t.bytes,t.hashed,t.repeated,t.files,t.unknown_bytes,t.unknown_files,t.entries,t."
            "errors FROM totals t JOIN dirs d ON d.id=t.dir WHERE d.path=? AND t.scope=?");
        if (s) {
            bind_text(s, 1, path);
            sqlite3_bind_int(s, 2, r->keep);
        }
        UsageTotals t = {0};
        if (s && sqlite3_step(s) == SQLITE_ROW)
            usage_totals_read(s, 0, &t);
        sqlite3_finalize(s);
        char logical[40], unique[40], repeated[40], unknown[40];
        human_bytes(t.bytes, logical);
        human_bytes(t.hashed - t.repeated, unique);
        human_bytes(t.repeated, repeated);
        human_bytes(t.unknown_bytes, unknown);
        badd(&b,
             "Logical: %s | Unique here: %s | Repeated here: %s\n"
             "Files: %lld | Unverified: %s (%lld files) | Errors: %lld\n\n"
             "Unique / repeated values cover verified nonempty content inside this subtree.\n"
             "Copies outside this subtree are not subtracted. Unique totals are not additive\n"
             "across sibling folders. Select a file to inspect its occurrences.\n",
             logical, unique, repeated, (long long)t.files, unknown, (long long)t.unknown_files,
             (long long)t.errors);
    } else if (strcmp(kind, "file")) {
        badd(&b, "%s: metadata-only entry. No file content is counted; links are not followed.\n",
             kind);
    } else if (size == 0 && verified_version && !pending && !*error) {
        badd(&b, "Empty file: independent metadata entry; excluded from duplicate groups.\n");
        if (*dest) {
            display = display_path(dest);
            badd(&b, "Destination: /%s | %s\n", display, state);
            free(display);
        } else
            badd(&b, "Destination: no indexed copy\n");
    } else if (pending || *error || strlen(hash) != HASH_LEN) {
        badd(&b, "UNVERIFIED: no current usable content hash.\n%s\n",
             *error ? error : "Observed metadata is waiting for verification.");
    } else {
        badd(&b, "SHA-512: %s\n", hash);
        if (*dest) {
            display = display_path(dest);
            badd(&b, "Destination: /%s | %s\n", display, state);
            free(display);
        } else
            badd(&b, "Destination: no indexed copy\n");
        badd(&b, "\nCURRENT SOURCE OCCURRENCES — page %d (all source entries)\n",
             r->occurrence_page + 1);
        s = sql_prepare(
            &db->cache,
            "SELECT e.path,e.discarded FROM src.versions v JOIN src.entries e ON e.current=v.id "
            "LEFT JOIN src.discoveries d ON d.eid=e.id WHERE v.hash=? AND e.present=1 AND "
            "e.kind='file' AND v.size>0 "
            "AND e.error='' AND d.eid IS NULL ORDER BY e.path COLLATE BINARY LIMIT 65 OFFSET ?");
        if (s) {
            bind_text(s, 1, hash);
            sqlite3_bind_int64(s, 2, (int64_t)r->occurrence_page * 64);
        }
        int count = 0;
        while (s && sqlite3_step(s) == SQLITE_ROW) {
            if (count++ == 64) {
                badd(&b, "More occurrences available: Next occurrences.\n");
                break;
            }
            display = display_path((const char *)sqlite3_column_text(s, 0));
            badd(&b, "/%s%s\n", display, sqlite3_column_int(s, 1) ? " [rejected]" : "");
            free(display);
        }
        if (!count)
            badd(&b, "No further occurrences on this page.\n");
        sqlite3_finalize(s);
    }
    free(kind);
    free(hash);
    free(error);
    free(dest);
    free(state);
    return b.s;
}
UsageResult *usage_query(UsageDb *db, const UsageRequest *r)
{
    Catalog *c = &db->cache;
    c->error[0] = 0;
    UsageResult *out = usage_cache_load(db, r);
    int cached = out != NULL;
    if (!out) {
        out = xmalloc(sizeof(*out));
        out->path = xstrdup(r->path);
        out->rows = xmalloc(USAGE_TREE_LIMIT * sizeof(*out->rows));
    }
    out->serial = db->executing;
    out->revision = db->indexed_revision;
    sqlite3_progress_handler(c->db, 1000, usage_query_interrupted, db);
    int failed = sql_exec(c, "BEGIN");
    sqlite3_stmt *s = sql_prepare(c, "SELECT revision FROM src.usage_clock WHERE id=1");
    if (!s || sqlite3_step(s) != SQLITE_ROW || sqlite3_column_int64(s, 0) != db->indexed_revision)
        failed = -1;
    sqlite3_finalize(s);
    if (!cached) {
        s = sql_prepare(c, "SELECT "
                           "t.bytes,t.hashed,t.repeated,t.files,t.unknown_bytes,t.unknown_files,t."
                           "entries,t.errors "
                           "FROM dirs d JOIN totals t ON t.dir=d.id WHERE d.path=? AND t.scope=?");
        if (s) {
            bind_text(s, 1, r->path);
            sqlite3_bind_int(s, 2, r->keep);
        }
        if (!s || sqlite3_step(s) != SQLITE_ROW)
            failed = -1;
        else
            usage_totals_read(s, 0, &out->totals);
        sqlite3_finalize(s);
        out->children = children_count(db, r->path, r->keep);
        if (out->children < 0)
            failed = -1;
        int last = out->children > 0 ? (int)((out->children - 1) / USAGE_PAGE) : 0;
        out->page = r->page > last ? last : r->page;
        if (!failed)
            failed = tree(db, r, out, r->path, out->page, 0);
        if (!failed)
            failed = chart(db, r, out, r->path, -1, 0, out->totals.bytes);
    }
    if (!failed)
        out->details = details(db, r, out);
    s = sql_prepare(c, "SELECT value FROM src.settings WHERE key='scan_complete'");
    out->complete =
        s && sqlite3_step(s) == SQLITE_ROW && !strcmp((const char *)sqlite3_column_text(s, 0), "1");
    sqlite3_finalize(s);
    sqlite3_progress_handler(c->db, 0, NULL, NULL);
    sql_rollback(c);
    if (usage_query_interrupted(db) || failed) {
        usage_result_free(out);
        return NULL;
    }
    if (!cached)
        usage_cache_store(db, r, out);
    return out;
}
