#include "usage_internal.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *fields[] = {"bytes",         "hashed",        "repeated", "files",
                               "unknown_bytes", "unknown_files", "entries",  "errors"};
static const char *valid_hash =
    "v.id IS NOT NULL AND e.error='' AND d.eid IS NULL AND length(v.hash)=128";

int usage_stmt_done(Catalog *c, sqlite3_stmt *s)
{
    if (!s)
        return -1;
    int rc = sqlite3_step(s);
    sqlite3_reset(s);
    sqlite3_clear_bindings(s);
    return rc == SQLITE_DONE ? 0 : sql_error(c, rc, "update usage cache");
}
void usage_totals_read(sqlite3_stmt *s, int col, UsageTotals *t)
{
    t->bytes = sqlite3_column_int64(s, col);
    t->hashed = sqlite3_column_int64(s, col + 1);
    t->repeated = sqlite3_column_int64(s, col + 2);
    t->files = sqlite3_column_int64(s, col + 3);
    t->unknown_bytes = sqlite3_column_int64(s, col + 4);
    t->unknown_files = sqlite3_column_int64(s, col + 5);
    t->entries = sqlite3_column_int64(s, col + 6);
    t->errors = sqlite3_column_int64(s, col + 7);
}
static char *readonly_uri(const char *path)
{
    Buf b = {0};
    badd(&b, "file:");
    for (const unsigned char *p = (const unsigned char *)path; *p; p++) {
        unsigned char c = *p;
#ifdef _WIN32
        if (c == '\\')
            c = '/';
#endif
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            strchr("/:._-", c))
            badd(&b, "%c", c);
        else
            badd(&b, "%%%02X", c);
    }
    badd(&b, "?mode=ro");
    return b.s;
}
int usage_database_open(UsageDb *db)
{
    Catalog *c = &db->cache;
    const char *names[] = {"usage.sqlite", "usage.sqlite-wal", "usage.sqlite-shm",
                           "usage.sqlite-journal"};
    for (unsigned i = 0; i < sizeof(names) / sizeof(*names); i++) {
        struct stat st;
        if (!fstatat(db->owner->mfd, names[i], &st, AT_SYMLINK_NOFOLLOW) && !S_ISREG(st.st_mode))
            return fail(c, "Unsafe analysis cache file: %s", names[i]);
    }
    char *path = join_path(db->owner->state_dir, "usage.sqlite");
    int rc = sqlite3_open_v2(
        path, &c->db,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX | SQLITE_OPEN_URI, NULL);
    free(path);
    c->log = db->owner->log;
    if (rc != SQLITE_OK)
        return sql_error(c, rc, "open usage cache");
    sqlite3_busy_timeout(c->db, 250);
    if (sql_exec(c, "PRAGMA journal_mode=WAL;PRAGMA synchronous=NORMAL;PRAGMA cache_size=-16384;"
                    "PRAGMA temp_store=FILE;PRAGMA wal_autocheckpoint=1000;"
                    "CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY,value TEXT);"))
        return -1;
    path = join_path(db->owner->state_dir, "catalog.sqlite");
    char *uri = readonly_uri(path);
    free(path);
    sqlite3_stmt *s = sql_prepare(c, "ATTACH DATABASE ? AS src");
    if (s)
        bind_text(s, 1, uri);
    free(uri);
    rc = usage_stmt_done(c, s);
    sqlite3_finalize(s);
    if (rc)
        return rc;
    s = sql_prepare(c, "SELECT value FROM meta WHERE key='revision'");
    if (s && sqlite3_step(s) == SQLITE_ROW)
        db->indexed_revision = sqlite3_column_int64(s, 0);
    sqlite3_finalize(s);
    s = sql_prepare(c, "SELECT value FROM meta WHERE key='identity'");
    if (!s || sqlite3_step(s) != SQLITE_ROW ||
        strcmp((const char *)sqlite3_column_text(s, 0), db->token))
        db->indexed_revision = 0;
    sqlite3_finalize(s);
    s = sql_prepare(c, "SELECT value FROM meta WHERE key='format'");
    if (!s || sqlite3_step(s) != SQLITE_ROW || sqlite3_column_int(s, 0) != 1)
        db->indexed_revision = 0;
    sqlite3_finalize(s);
    return 0;
}
static int create_work(UsageDb *db)
{
    return sql_exec(
        &db->cache,
        "DROP TABLE IF EXISTS next_dirs;DROP TABLE IF EXISTS next_direct;"
        "DROP TABLE IF EXISTS next_totals;DROP TABLE IF EXISTS next_groups;DROP TABLE IF EXISTS "
        "next_terms;"
        "CREATE TABLE next_dirs(id INTEGER PRIMARY KEY,path TEXT NOT NULL UNIQUE,parent_path TEXT,"
        "parent INTEGER,depth INTEGER,error INTEGER DEFAULT 0);"
        "INSERT INTO next_dirs VALUES(0,'','',NULL,0,0);"
        "CREATE TABLE next_direct(dir INTEGER,scope INTEGER,bytes INTEGER DEFAULT 0,hashed INTEGER "
        "DEFAULT 0,"
        "repeated INTEGER DEFAULT 0,files INTEGER DEFAULT 0,unknown_bytes INTEGER DEFAULT 0,"
        "unknown_files INTEGER DEFAULT 0,entries INTEGER DEFAULT 0,errors INTEGER DEFAULT "
        "0,PRIMARY KEY(dir,scope)) WITHOUT ROWID;"
        "CREATE TABLE next_totals AS SELECT * FROM next_direct WHERE 0;"
        "CREATE UNIQUE INDEX next_totals_key ON next_totals(dir,scope);"
        "CREATE TABLE next_groups(id INTEGER PRIMARY KEY,hash TEXT NOT NULL UNIQUE);"
        "CREATE TABLE next_terms(gid INTEGER,dir INTEGER,scope INTEGER,bytes INTEGER NOT "
        "NULL,PRIMARY KEY(gid,dir,scope)) WITHOUT ROWID;"
        "CREATE INDEX next_terms_dir ON next_terms(dir,scope);");
}
static int64_t chunk_end(UsageDb *db, int64_t last)
{
    sqlite3_stmt *s = sql_prepare(&db->cache, "SELECT MAX(id),COUNT(*) FROM (SELECT id FROM "
                                              "src.entries WHERE id>? ORDER BY id LIMIT 20000)");
    if (!s)
        return -1;
    sqlite3_bind_int64(s, 1, last);
    int rc = sqlite3_step(s);
    int64_t end = 0;
    if (rc == SQLITE_ROW) {
        end = sqlite3_column_int64(s, 0);
        pthread_mutex_lock(&db->mutex);
        db->status.done += sqlite3_column_int64(s, 1);
        pthread_mutex_unlock(&db->mutex);
    }
    sqlite3_finalize(s);
    return rc == SQLITE_ROW ? end : -1;
}
static int load_directories(UsageDb *db)
{
    Catalog *c = &db->cache;
    usage_phase(db, USAGE_BUILDING, "Indexing directory relationships");
    sqlite3_stmt *s = sql_prepare(
        c,
        "INSERT INTO next_dirs(id,path,parent_path,depth,error) SELECT id,path,COALESCE(parent,''),"
        "length(path)-length(replace(path,'/',''))+1,(present=1 AND error<>'') FROM src.entries "
        "WHERE id>? AND id<=? AND kind='directory' AND path<>''");
    if (!s)
        return -1;
    int64_t last = 0, end = 0;
    int rc = 0;
    while (!usage_interrupted(db) && (end = chunk_end(db, last)) > 0) {
        sqlite3_bind_int64(s, 1, last);
        sqlite3_bind_int64(s, 2, end);
        if (usage_stmt_done(c, s)) {
            rc = -1;
            break;
        }
        last = end;
    }
    sqlite3_finalize(s);
    if (usage_interrupted(db) || end < 0)
        return -1;
    if (rc)
        return rc;
    /* Missing historical parents may remain in the catalog. Each source path is
     * linked by its lexical parent, never by a recursive filesystem operation. */
    return sql_exec(c, "UPDATE next_dirs SET parent=COALESCE((SELECT p.id FROM next_dirs p WHERE "
                       "p.path=next_dirs.parent_path),0) WHERE id<>0;"
                       "UPDATE next_dirs SET error=COALESCE((SELECT present=1 AND error<>'' FROM "
                       "src.entries WHERE path=''),0) WHERE id=0;"
                       "CREATE INDEX next_dirs_parent ON next_dirs(parent);CREATE INDEX "
                       "next_dirs_depth ON next_dirs(depth DESC);"
                       "INSERT INTO next_direct(dir,scope) SELECT id,0 FROM next_dirs UNION ALL "
                       "SELECT id,1 FROM next_dirs;");
}
/* Aggregate each bounded range of catalog IDs. Committing between ranges releases
 * source snapshots, so a background index does not retain an entire scan's WAL. */
static char *direct_sql(int partial, int scope)
{
    return fmt(
        "SELECT e.parent,COALESCE(SUM(CASE WHEN e.kind='file' THEN "
        "MAX(0,COALESCE(d.size,v.size,0)) ELSE 0 END),0),"
        "COALESCE(SUM(CASE WHEN e.kind='file' AND v.size>0 AND (%s) THEN v.size ELSE 0 END),0),0,"
        "COALESCE(SUM(e.kind='file'),0),"
        "COALESCE(SUM(CASE WHEN e.kind='file' AND NOT (v.id IS NOT NULL AND e.error='' AND d.eid "
        "IS NULL AND (v.size=0 OR COALESCE(length(v.hash),0)=128)) THEN "
        "MAX(0,COALESCE(d.size,v.size,0)) ELSE 0 END),0),"
        "COALESCE(SUM(e.kind='file' AND NOT (v.id IS NOT NULL AND e.error='' AND d.eid IS NULL AND "
        "(v.size=0 OR COALESCE(length(v.hash),0)=128))),0),"
        "COUNT(*),COALESCE(SUM(e.kind<>'directory' AND e.error<>''),0) "
        "FROM src.entries e LEFT JOIN src.versions v ON v.id=e.current LEFT JOIN src.discoveries d "
        "ON d.eid=e.id "
        "WHERE e.present=1 AND e.path<>'' AND (%d=0 OR e.kind='directory' OR e.discarded=0) AND %s "
        "GROUP BY e.parent",
        valid_hash, scope, partial ? "e.parent=?" : "e.id>? AND e.id<=?");
}
static int direct_range(UsageDb *db, int64_t start, int64_t end, const char *path)
{
    Catalog *c = &db->cache;
    const char *p = db->full_build ? "next_" : "";
    Buf update = {0};
    badd(&update, "UPDATE %sdirect SET ", p);
    for (int i = 0; i < 8; i++)
        badd(&update, "%s%s=%s+?%d", i ? "," : "", fields[i], fields[i], i + 1);
    badd(&update, " WHERE dir=(SELECT id FROM %sdirs WHERE path=?9) AND scope=?10", p);
    sqlite3_stmt *up = sql_prepare(c, update.s);
    bfree(&update);
    if (!up)
        return -1;
    int failed = 0;
    for (int scope = 0; scope < 2 && !failed; scope++) {
        char *sql = direct_sql(path != NULL, scope);
        sqlite3_stmt *s = sql_prepare(c, sql);
        free(sql);
        if (!s) {
            failed = -1;
            break;
        }
        if (path)
            bind_text(s, 1, path);
        else {
            sqlite3_bind_int64(s, 1, start);
            sqlite3_bind_int64(s, 2, end);
        }
        int rc;
        while ((rc = sqlite3_step(s)) == SQLITE_ROW) {
            for (int j = 0; j < 8; j++)
                sqlite3_bind_int64(up, j + 1, sqlite3_column_int64(s, j + 1));
            bind_text(up, 9, (const char *)sqlite3_column_text(s, 0));
            sqlite3_bind_int(up, 10, scope);
            if (usage_stmt_done(c, up)) {
                failed = -1;
                break;
            }
        }
        if (rc != SQLITE_DONE)
            failed = -1;
        sqlite3_finalize(s);
    }
    sqlite3_finalize(up);
    return failed;
}
static int load_direct(UsageDb *db)
{
    usage_phase(db, USAGE_BUILDING, "Summing logical sizes from the catalog");
    int64_t last = 0, end = 0;
    while (!usage_interrupted(db) && (end = chunk_end(db, last)) > 0) {
        if (sql_exec(&db->cache, "BEGIN"))
            return -1;
        int rc = direct_range(db, last, end, NULL);
        if (sql_end(&db->cache, rc))
            return -1;
        last = end;
    }
    return end < 0 || usage_interrupted(db) ? -1 : 0;
}
int usage_rollup(UsageDb *db)
{
    Catalog *c = &db->cache;
    const char *p = db->full_build ? "next_" : "";
    usage_phase(db, USAGE_BUILDING, "Rolling up directory totals");
    char *sql;
    if (db->full_build) {
        sql = fmt("INSERT INTO next_totals SELECT * FROM next_direct;"
                  "UPDATE next_totals SET repeated=COALESCE((SELECT SUM(bytes) FROM next_terms t "
                  "WHERE t.dir=next_totals.dir AND t.scope=next_totals.scope),0),"
                  "errors=errors+(SELECT error FROM next_dirs d WHERE d.id=dir)");
        int rc = sql_exec(c, sql);
        free(sql);
        if (rc)
            return rc;
    }
    sql = fmt("SELECT id,parent FROM %sdirs %s ORDER BY depth DESC", p,
              db->full_build ? "" : "WHERE id IN (SELECT id FROM dirty_nodes)");
    sqlite3_stmt *rows = sql_prepare(c, sql);
    free(sql);
    if (!rows)
        return -1;
    Buf b = {0};
    badd(&b, "UPDATE %stotals SET ", p);
    for (int i = 0; i < 8; i++) {
        if (db->full_build)
            badd(&b, "%s%s=%s+COALESCE((SELECT %s FROM %stotals WHERE dir=?1 AND scope=?2),0)",
                 i ? "," : "", fields[i], fields[i], fields[i], p);
        else {
            badd(&b,
                 "%s%s=COALESCE((SELECT %s FROM direct WHERE dir=?1 AND scope=?2),0)+"
                 "COALESCE((SELECT SUM(t.%s) FROM dirs d JOIN totals t ON t.dir=d.id AND "
                 "t.scope=?2 WHERE d.parent=?1),0)",
                 i ? "," : "", fields[i], fields[i], fields[i]);
            if (i == 2)
                badd(&b, "+COALESCE((SELECT SUM(bytes) FROM terms WHERE dir=?1 AND scope=?2),0)");
            if (i == 7)
                badd(&b, "+(SELECT error FROM dirs WHERE id=?1)");
        }
    }
    badd(&b, " WHERE dir=?%d AND scope=?2", db->full_build ? 3 : 1);
    sqlite3_stmt *up = sql_prepare(c, b.s);
    bfree(&b);
    int rc = SQLITE_DONE, failed = up ? 0 : -1;
    while (!failed && !usage_interrupted(db) && (rc = sqlite3_step(rows)) == SQLITE_ROW) {
        int64_t id = sqlite3_column_int64(rows, 0), parent = sqlite3_column_int64(rows, 1);
        if (db->full_build && id == 0)
            continue;
        for (int scope = 0; scope < 2; scope++) {
            sqlite3_bind_int64(up, 1, id);
            sqlite3_bind_int(up, 2, scope);
            if (db->full_build)
                sqlite3_bind_int64(up, 3, parent);
            if (usage_stmt_done(c, up)) {
                failed = -1;
                break;
            }
        }
    }
    sqlite3_finalize(up);
    sqlite3_finalize(rows);
    return failed || rc != SQLITE_DONE || usage_interrupted(db) ? -1 : 0;
}
static int incremental_direct(UsageDb *db)
{
    Catalog *c = &db->cache;
    if (sql_exec(c, "DROP TABLE IF EXISTS temp.dirty_nodes;CREATE TEMP TABLE dirty_nodes(id "
                    "INTEGER PRIMARY KEY);"
                    "INSERT OR IGNORE INTO dirty_nodes SELECT d.id FROM dirs d JOIN "
                    "src.usage_dirty_dirs q ON q.path=d.path;"))
        return -1;
    sqlite3_stmt *s = sql_prepare(c, "SELECT path FROM src.usage_dirty_dirs");
    if (!s)
        return -1;
    int rc = SQLITE_DONE, failed = 0;
    while (!usage_interrupted(db) && (rc = sqlite3_step(s)) == SQLITE_ROW) {
        char *path = xstrdup((const char *)sqlite3_column_text(s, 0));
        Buf sql = {0};
        badd(&sql, "UPDATE direct SET ");
        for (int i = 0; i < 8; i++)
            badd(&sql, "%s%s=0", i ? "," : "", fields[i]);
        badd(&sql, " WHERE dir=(SELECT id FROM dirs WHERE path=?)");
        sqlite3_stmt *zero = sql_prepare(c, sql.s);
        bfree(&sql);
        if (zero)
            bind_text(zero, 1, path);
        failed = usage_stmt_done(c, zero);
        sqlite3_finalize(zero);
        if (!failed)
            failed = direct_range(db, 0, 0, path);
        free(path);
        if (failed)
            break;
    }
    sqlite3_finalize(s);
    if (failed || rc != SQLITE_DONE || usage_interrupted(db))
        return -1;
    return sql_exec(
        c, "UPDATE dirs SET error=COALESCE((SELECT present=1 AND error<>'' FROM src.entries e "
           "WHERE e.path=dirs.path),0) WHERE id IN (SELECT id FROM dirty_nodes);");
}
static int publish(UsageDb *db, int64_t revision)
{
    Catalog *c = &db->cache;
    if (db->full_build) {
        const char *tables[] = {"dirs", "direct", "totals", "groups", "terms"};
        for (unsigned i = 0; i < 5; i++) {
            char *sql = fmt("DROP TABLE IF EXISTS %s;ALTER TABLE next_%s RENAME TO %s", tables[i],
                            tables[i], tables[i]);
            int rc = sql_exec(c, sql);
            free(sql);
            if (rc)
                return rc;
        }
        /* Named indexes belong to the newly published generation. */
        if (sql_exec(
                c, "DROP INDEX next_totals_key;CREATE UNIQUE INDEX totals_key ON totals(dir,scope);"
                   "DROP INDEX next_dirs_parent;CREATE INDEX dirs_parent ON dirs(parent);"
                   "DROP INDEX next_dirs_depth;CREATE INDEX dirs_depth ON dirs(depth DESC);"
                   "DROP INDEX next_terms_dir;CREATE INDEX terms_dir ON terms(dir,scope);"))
            return -1;
    }
    sqlite3_stmt *s = sql_prepare(c, "INSERT INTO meta VALUES('revision',?) ON CONFLICT(key) DO "
                                     "UPDATE SET value=excluded.value");
    if (s)
        sqlite3_bind_int64(s, 1, revision);
    int rc = usage_stmt_done(c, s);
    sqlite3_finalize(s);
    if (rc)
        return rc;
    s = sql_prepare(c, "INSERT INTO meta VALUES('identity',?) ON CONFLICT(key) DO UPDATE SET "
                       "value=excluded.value");
    if (s)
        bind_text(s, 1, db->token);
    rc = usage_stmt_done(c, s);
    sqlite3_finalize(s);
    if (rc)
        return rc;
    return sql_exec(c, "INSERT OR REPLACE INTO meta VALUES('format','1');");
}
int usage_index(UsageDb *db, int force)
{
    if (usage_clock(db))
        return -1;
    int64_t revision = db->revision;
    if (!force && db->indexed_revision == revision)
        return 0;
    pthread_mutex_lock(&db->mutex);
    db->started = monotonic_ns();
    pthread_mutex_unlock(&db->mutex);
    db->full_build = force || !db->indexed_revision || db->full > db->indexed_revision;
    Catalog *c = &db->cache;
    int rc = 0;
    c->error[0] = 0;
    sqlite3_progress_handler(c->db, 2000, usage_interrupted, db);
    if (db->full_build) {
        rc = create_work(db);
        if (!rc)
            rc = load_directories(db);
        if (!rc)
            rc = load_direct(db);
        if (!rc)
            rc = usage_duplicate_build(db, 1);
        if (!rc)
            rc = sql_exec(c, "BEGIN");
    } else {
        usage_phase(db, USAGE_BUILDING, "Updating changed directories and duplicate groups");
        rc = sql_exec(c, "BEGIN");
        if (!rc)
            rc = incremental_direct(db);
        if (!rc)
            rc = usage_duplicate_build(db, 0);
        if (!rc)
            rc = sql_exec(
                c, "WITH RECURSIVE ancestors(id) AS (SELECT id FROM dirty_nodes UNION SELECT "
                   "d.parent FROM dirs d JOIN ancestors a ON d.id=a.id WHERE d.parent IS NOT NULL) "
                   "INSERT OR IGNORE INTO dirty_nodes SELECT id FROM ancestors;");
    }
    if (!rc)
        rc = usage_rollup(db);
    if (!rc && usage_clock(db))
        rc = -1;
    if (!rc && db->revision != revision)
        rc = -2;
    if (!rc && usage_interrupted(db))
        rc = -2;
    if (!rc)
        rc = publish(db, revision);
    int cancelled_generation = rc == -2;
    int sqlite_error = sqlite3_errcode(c->db);
    sqlite3_progress_handler(c->db, 0, NULL, NULL);
    rc = sql_end(c, rc);
    if (rc)
        return cancelled_generation                                           ? -2
               : sqlite_error == SQLITE_BUSY || sqlite_error == SQLITE_LOCKED ? -3
                                                                              : -1;
    db->indexed_revision = revision;
    /* Do not lose changes made after the snapshot used for this generation. */
    char *sql = fmt("BEGIN IMMEDIATE;DELETE FROM usage_dirty_dirs WHERE revision<=%lld;"
                    "DELETE FROM usage_dirty_hashes WHERE revision<=%lld;UPDATE usage_clock SET "
                    "pending=0 WHERE revision=%lld;COMMIT;",
                    (long long)revision, (long long)revision, (long long)revision);
    int cleanup = sql_exec(&db->catalog, sql);
    free(sql);
    if (cleanup)
        sql_rollback(&db->catalog); /* Queues are safely reusable. */
    sqlite3_wal_checkpoint_v2(c->db, NULL, SQLITE_CHECKPOINT_PASSIVE, NULL, NULL);
    activity_log(db->owner, LOG_INFO, "usage.index", NULL, NULL,
                 "%s logical-usage index ready at catalog revision %lld",
                 db->full_build ? "Complete" : "Incremental", (long long)revision);
    usage_phase(db, USAGE_READY, "Logical usage index ready");
    return 0;
}
