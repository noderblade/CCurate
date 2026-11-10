/* Fault injection uses disposable catalogs, never an existing user session. */
#include "curator.h"
#include "remove_target.h"
#undef NDEBUG
#include <assert.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int commits;
static int deny_commit(void *arg, int op, const char *value, const char *b, const char *db,
                       const char *trigger)
{
    (void)b;
    (void)db;
    (void)trigger;
    if (op == SQLITE_TRANSACTION && value && !strcmp(value, "COMMIT") && ++commits == *(int *)arg)
        return SQLITE_DENY;
    return SQLITE_OK;
}
static int interrupt_query(void *arg)
{
    (void)arg;
    return 1;
}
static void writer_available(Catalog *c)
{
    assert(sqlite3_get_autocommit(c->db));
    sqlite3 *other = NULL;
    assert(sqlite3_open(sqlite3_db_filename(c->db, "main"), &other) == SQLITE_OK);
    assert(sqlite3_exec(other, "BEGIN IMMEDIATE;ROLLBACK", NULL, NULL, NULL) == SQLITE_OK);
    assert(sqlite3_close(other) == SQLITE_OK);
}
static void manifest_hash(Catalog *c, char hash[129])
{
    int fd = openat(c->mfd, "archive.jsonl", O_RDONLY | O_CLOEXEC);
    assert(fd >= 0);
    assert(!hash_fd(c, fd, hash, NULL));
    close(fd);
}
int main(int argc, char **argv)
{
    assert(argc == 3);
    Catalog c;
    assert(!catalog_open(&c, argv[1], argv[2], 0));
    int deny_at = 1;
    sqlite3_set_authorizer(c.db, deny_commit, &deny_at);
    assert(prepare_hash_groups(&c) == -1);
    writer_available(&c);
    sqlite3_set_authorizer(c.db, NULL, NULL);
    char *ready = setting(&c, "hash_groups_ready", "0");
    assert(!strcmp(ready, "0"));
    free(ready);
    assert(!prepare_hash_groups(&c));
    puts("failed occurrence-cache commit releases the writer and retries: PASS");

    assert(!sql_exec(&c, "BEGIN IMMEDIATE"));
    assert(!set_setting(&c, "test_uncommitted", "discard"));
    sqlite3_progress_handler(c.db, 1, interrupt_query, NULL);
    assert(sqlite3_exec(c.db,
                        "WITH RECURSIVE n(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM n "
                        "WHERE x<10000) SELECT sum(x) FROM n",
                        NULL, NULL, NULL) == SQLITE_INTERRUPT);
    assert(sql_end(&c, -1) == -1);
    writer_available(&c);
    ready = setting(&c, "test_uncommitted", "absent");
    assert(!strcmp(ready, "absent"));
    free(ready);
    puts("cancelled SQL cannot cancel transaction cleanup: PASS");

    /* A real COMMIT/BUSY: a rollback-journal reader blocks the writer's commit. */
    assert(!sql_exec(&c, "PRAGMA journal_mode=DELETE"));
    sqlite3 *reader = NULL;
    assert(sqlite3_open(sqlite3_db_filename(c.db, "main"), &reader) == SQLITE_OK);
    assert(!sql_exec(&c, "BEGIN IMMEDIATE"));
    assert(!set_setting(&c, "test_uncommitted", "discard"));
    assert(sqlite3_exec(reader, "BEGIN;SELECT * FROM settings", NULL, NULL, NULL) == SQLITE_OK);
    sqlite3_busy_timeout(c.db, 0);
    assert(sql_end(&c, 0) == -1);
    assert(strstr(c.error, "SQLite busy [5/5] during COMMIT"));
    assert(sqlite3_get_autocommit(c.db));
    assert(sqlite3_exec(reader, "ROLLBACK;BEGIN IMMEDIATE;ROLLBACK", NULL, NULL, NULL) ==
           SQLITE_OK);
    assert(sqlite3_close(reader) == SQLITE_OK);
    assert(!sql_exec(&c, "PRAGMA journal_mode=WAL"));
    sqlite3_busy_timeout(c.db, 10000);
    puts("real COMMIT contention reports the operation and releases the writer: PASS");

    /* Fail after a previously committed inventory batch, then retry on the same connection. */
    deny_at = 3;
    commits = 0;
    sqlite3_set_authorizer(c.db, deny_commit, &deny_at);
    assert(scan_source(&c, 0) == -1);
    assert(commits == deny_at && atomic_load(&c.scan.phase) == SCAN_STOPPED);
    writer_available(&c);
    sqlite3_set_authorizer(c.db, NULL, NULL);
    ready = setting(&c, "scan_complete", "");
    assert(!strcmp(ready, "0"));
    free(ready);
    assert(!scan_source(&c, 0));
    assert(atomic_load(&c.scan.phase) == SCAN_DONE);
    puts("failed scan batch stops cleanly and the same session can rescan: PASS");

    char before[129], after[129];
    manifest_hash(&c, before);
    deny_at = 1;
    commits = 0;
    sqlite3_set_authorizer(c.db, deny_commit, &deny_at);
    assert(export_archive(&c) == -1);
    writer_available(&c);
    sqlite3_set_authorizer(c.db, NULL, NULL);
    manifest_hash(&c, after);
    assert(!strcmp(before, after));
    assert(!export_archive(&c));
    puts("failed export preserves the previous portable index: PASS");

    assert(!copy_entry(&c, "original.txt", "Removal", 0));
    atomic_store(&c.cancel, 1);
    assert(remove_target_copy(&c, "Removal/original.txt") == -1);
    atomic_store(&c.cancel, 0);
    int fd = open_beneath(c.dfd, "Removal/original.txt", O_RDONLY, 0);
    assert(fd >= 0);
    close(fd);

    commits = 0;
    deny_at = 1;
    sqlite3_set_authorizer(c.db, deny_commit, &deny_at);
    assert(remove_target_copy(&c, "Removal/original.txt") == -1);
    sqlite3_set_authorizer(c.db, NULL, NULL);
    writer_available(&c);
    fd = open_beneath(c.dfd, "Removal/original.txt", O_RDONLY, 0);
    assert(fd >= 0);
    close(fd);
    Entry entry = {0};
    assert(!load_entry(&c, "original.txt", &entry, 0));
    assert(!strcmp(entry.state, "verified"));
    entry_free(&entry);
    puts("cancelled removal / failed intent commit leave the destination intact: PASS");

    commits = 0;
    deny_at = 2;
    sqlite3_set_authorizer(c.db, deny_commit, &deny_at);
    assert(remove_target_copy(&c, "Removal/original.txt") == -1);
    sqlite3_set_authorizer(c.db, NULL, NULL);
    writer_available(&c);
    assert(open_beneath(c.dfd, "Removal/original.txt", O_RDONLY, 0) < 0);
    assert(!load_entry(&c, "original.txt", &entry, 0));
    assert(strcmp(entry.state, "verified"));
    entry_free(&entry);
    assert(!recover_operations(&c));
    assert(!load_entry(&c, "original.txt", &entry, 0));
    assert(!*entry.dest && !strcmp(entry.state, "missing"));
    entry_free(&entry);
    assert(!copy_entry(&c, "original.txt", "Recurated", 0));
    puts("failed removal commit leaves a recoverable journal and never false verification: PASS");

    commits = 0;
    deny_at = 3; /* intent, mapping, then the JSONL export snapshot */
    sqlite3_set_authorizer(c.db, deny_commit, &deny_at);
    assert(remove_target_copy(&c, "Recurated/original.txt") == -1);
    sqlite3_set_authorizer(c.db, NULL, NULL);
    writer_available(&c);
    sqlite3_stmt *pending = sql_prepare(&c, "SELECT COUNT(*) FROM operations WHERE kind='remove' "
                                            "AND state='remove_export'");
    assert(sqlite3_step(pending) == SQLITE_ROW && sqlite3_column_int(pending, 0) == 1);
    sqlite3_finalize(pending);
    assert(!recover_operations(&c));
    pending = sql_prepare(&c, "SELECT COUNT(*) FROM operations WHERE kind='remove' "
                              "AND state='remove_export'");
    assert(sqlite3_step(pending) == SQLITE_ROW && sqlite3_column_int(pending, 0) == 0);
    sqlite3_finalize(pending);
    puts("failed removal export stays pending until JSONL is repaired: PASS");
    catalog_close(&c);
    return 0;
}
