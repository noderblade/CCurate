#include "usage_internal.h"
#include <stdlib.h>
#include <string.h>

/* These triggers track catalog facts, not UI settings or destination operations.
 * Coalesced queues are bounded: large changes request one complete rebuild. */
static void tick(Buf *b, const char *topology)
{
    badd(b,
         "UPDATE usage_clock SET revision=revision+1,pending=pending+1,"
         "full=CASE WHEN pending>=100000 OR (%s) THEN revision+1 ELSE full END;",
         topology);
}
static void dirty_parent(Buf *b, const char *expression)
{
    badd(b,
         "INSERT INTO usage_dirty_dirs(path,revision) SELECT COALESCE(%s,''),revision "
         "FROM usage_clock WHERE pending<=100000 ON CONFLICT(path) DO UPDATE SET "
         "revision=excluded.revision;",
         expression);
}
static void dirty_hash(Buf *b, const char *expression)
{
    badd(b,
         "INSERT INTO usage_dirty_hashes(hash,revision) SELECT %s,revision FROM usage_clock "
         "WHERE pending<=100000 AND (%s) IS NOT NULL ON CONFLICT(hash) DO UPDATE SET "
         "revision=excluded.revision;",
         expression, expression);
}
static int install_entry_triggers(Catalog *c)
{
    const char *events[] = {"INSERT", "UPDATE OF current,present,error,discarded,kind,parent,path",
                            "DELETE"};
    const char *names[] = {"insert", "update", "delete"};
    for (int i = 0; i < 3; i++) {
        Buf b = {0};
        badd(&b, "CREATE TRIGGER IF NOT EXISTS usage_entry_%s AFTER %s ON entries ", names[i],
             events[i]);
        if (i == 1)
            badd(&b, "WHEN OLD.current IS NOT NEW.current OR OLD.present IS NOT NEW.present "
                     "OR OLD.error IS NOT NEW.error OR OLD.discarded IS NOT NEW.discarded "
                     "OR OLD.kind IS NOT NEW.kind OR OLD.parent IS NOT NEW.parent OR OLD.path IS "
                     "NOT NEW.path ");
        badd(&b, "BEGIN ");
        tick(&b,
             i == 0 ? "NEW.kind='directory'"
             : i == 2
                 ? "OLD.kind='directory'"
                 : "(OLD.kind='directory' OR NEW.kind='directory') AND (OLD.kind IS NOT NEW.kind "
                   "OR OLD.path IS NOT NEW.path OR OLD.parent IS NOT NEW.parent OR OLD.present IS "
                   "NOT NEW.present)");
        if (i != 0) {
            dirty_parent(&b, "OLD.parent");
            dirty_hash(&b, "(SELECT hash FROM versions WHERE id=OLD.current AND size>0)");
        }
        if (i != 2) {
            dirty_parent(&b, "NEW.parent");
            dirty_hash(&b, "(SELECT hash FROM versions WHERE id=NEW.current AND size>0)");
            /* A directory's own error also changes its subtree's completeness. */
            dirty_parent(&b, "CASE WHEN NEW.kind='directory' THEN NEW.path ELSE NEW.parent END");
        }
        badd(&b, "END;");
        int rc = sql_exec(c, b.s);
        bfree(&b);
        if (rc)
            return rc;
    }
    return 0;
}
int usage_tracking_open(UsageDb *db)
{
    Catalog *c = &db->catalog;
    char *path = join_path(db->owner->state_dir, "catalog.sqlite");
    int rc = sqlite3_open_v2(path, &c->db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX, NULL);
    free(path);
    if (rc != SQLITE_OK)
        return sql_error(c, rc, "open usage catalog reader");
    c->log = db->owner->log;
    sqlite3_busy_timeout(c->db, 250);
    if (sql_exec(c, "PRAGMA cache_size=-16384;PRAGMA temp_store=FILE;BEGIN IMMEDIATE;"
                    "CREATE TABLE IF NOT EXISTS usage_clock(id INTEGER PRIMARY KEY CHECK(id=1),"
                    "revision INTEGER NOT NULL,full INTEGER NOT NULL,pending INTEGER NOT "
                    "NULL,token TEXT NOT NULL);"
                    "INSERT OR IGNORE INTO usage_clock VALUES(1,1,1,0,lower(hex(randomblob(16))));"
                    "CREATE TABLE IF NOT EXISTS usage_dirty_dirs(path TEXT PRIMARY KEY,revision "
                    "INTEGER NOT NULL);"
                    "CREATE TABLE IF NOT EXISTS usage_dirty_hashes(hash TEXT PRIMARY KEY,revision "
                    "INTEGER NOT NULL);"))
        return sql_end(c, -1);
    if (install_entry_triggers(c))
        return sql_end(c, -1);
    const char *events[] = {"INSERT", "UPDATE", "DELETE"};
    for (int i = 0; i < 3; i++) {
        Buf b = {0};
        badd(&b, "CREATE TRIGGER IF NOT EXISTS usage_discovery_%d AFTER %s ON discoveries BEGIN ",
             i, events[i]);
        tick(&b, "0");
        const char *which = i == 2 ? "OLD" : "NEW";
        char *parent = fmt("(SELECT parent FROM entries WHERE id=%s.eid)", which);
        char *hash = fmt("(SELECT v.hash FROM entries e JOIN versions v ON v.id=e.current WHERE "
                         "e.id=%s.eid AND v.size>0)",
                         which);
        dirty_parent(&b, parent);
        dirty_hash(&b, hash);
        badd(&b, "END;");
        free(parent);
        free(hash);
        int failed = sql_exec(c, b.s);
        bfree(&b);
        if (failed)
            return sql_end(c, -1);
    }
    Buf b = {0};
    badd(&b,
         "CREATE TRIGGER IF NOT EXISTS usage_version_update AFTER UPDATE OF size,hash ON versions "
         "WHEN (OLD.size IS NOT NEW.size OR OLD.hash IS NOT NEW.hash) AND EXISTS(SELECT 1 FROM "
         "entries WHERE current=NEW.id) BEGIN ");
    tick(&b, "0");
    dirty_parent(&b, "(SELECT parent FROM entries WHERE current=NEW.id LIMIT 1)");
    dirty_hash(&b, "OLD.hash");
    dirty_hash(&b, "NEW.hash");
    badd(&b, "END;");
    rc = sql_exec(c, b.s);
    bfree(&b);
    return sql_end(c, rc);
}
int usage_clock(UsageDb *db)
{
    sqlite3_stmt *s =
        sql_prepare(&db->catalog, "SELECT revision,full,pending,token FROM usage_clock WHERE id=1");
    if (!s)
        return -1;
    int rc = sqlite3_step(s);
    if (rc == SQLITE_ROW) {
        db->revision = sqlite3_column_int64(s, 0);
        db->full = sqlite3_column_int64(s, 1);
        db->pending = sqlite3_column_int64(s, 2);
        snprintf(db->token, sizeof(db->token), "%s", sqlite3_column_text(s, 3));
    }
    sqlite3_finalize(s);
    return rc == SQLITE_ROW ? 0 : sql_error(&db->catalog, rc, "read usage revision");
}
