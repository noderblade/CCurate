/* Deterministic filesystem remount/replacement simulation; no production hooks. */
#include "source_filesystem.h"
#undef NDEBUG
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *identity = "zfs:1234567890123456789:9876543210987654321";
static int probes;
int source_filesystem_probe(int parent, const char *leaf, const struct stat *st, char **guid,
                            char **name)
{
    (void)leaf;
    (void)st;
    probes++;
    *guid = identity ? xstrdup(identity) : NULL;
    *name = xstrdup("fixture/dataset");
    return dup(parent);
}
static int64_t number(Catalog *c, const char *query)
{
    sqlite3_stmt *s = sql_prepare(c, query);
    assert(s && sqlite3_step(s) == SQLITE_ROW);
    int64_t result = sqlite3_column_int64(s, 0);
    sqlite3_finalize(s);
    return result;
}
static void flags(Catalog *c)
{
    assert(number(c, "SELECT reviewed=1 AND discarded=1 FROM entries WHERE path='file'"));
}
int main(int argc, char **argv)
{
    assert(argc == 3);
    Catalog c;
    assert(!catalog_open(&c, argv[1], argv[2], 0));
    assert(!scan_source(&c, 0));
    int64_t count = number(&c, "SELECT COUNT(*) FROM versions");
    int64_t first = number(&c, "SELECT current FROM entries WHERE path='file'");
    assert(atomic_load(&c.bytes) > 0 && probes == 1);
    assert(!sql_exec(&c, "UPDATE versions SET dev=53;UPDATE entries SET reviewed=1,discarded=1"));
    catalog_close(&c);
    /* The real source is unchanged; the saved legacy dev no longer matches. */
    assert(!catalog_open(&c, argv[1], argv[2], 0));
    assert(!scan_source(&c, 0));
    assert(!atomic_load(&c.bytes));
    assert(count == number(&c, "SELECT COUNT(*) FROM versions"));
    flags(&c);
    assert(number(&c, "SELECT COUNT(*) FROM source_filesystems") == 1);
    puts("PASS runtime device changes preserve versions, cached hashes and decisions");
    assert(!sql_exec(&c, "UPDATE versions SET filesystem=NULL,dev=54"));
    catalog_close(&c);
    assert(!catalog_open(&c, argv[1], argv[2], 0));
    assert(!scan_source(&c, 0));
    assert(atomic_load(&c.bytes) > 0);
    assert(first == number(&c, "SELECT current FROM entries WHERE path='file'"));
    assert(count == number(&c, "SELECT COUNT(*) FROM versions"));
    flags(&c);
    puts("PASS unknown legacy identity is verified and bound without a new version");
    source_filesystems_close(&c);
    identity = "zfs:1234567890123456789:2222222222222222222";
    atomic_store(&c.bytes, 0);
    assert(!scan_source(&c, 0));
    assert(atomic_load(&c.bytes) > 0);
    assert(first != number(&c, "SELECT current FROM entries WHERE path='file'"));
    assert(number(&c, "SELECT COUNT(*) FROM source_filesystems") == 2);
    assert(!number(&c, "SELECT reviewed OR discarded FROM entries WHERE path='file'"));
    puts("PASS different persistent GUID triggers verification and a real revision");
    source_filesystems_close(&c);
    identity = NULL;
    count = number(&c, "SELECT COUNT(*) FROM versions");
    assert(!sql_exec(&c, "UPDATE entries SET reviewed=1,discarded=1"));
    assert(!scan_source(&c, 0));
    assert(count == number(&c, "SELECT COUNT(*) FROM versions"));
    flags(&c);
    assert(!sql_exec(&c, "UPDATE entries SET discarded=0"));
    atomic_store(&c.bytes, 0);
    assert(!copy_entry(&c, "file", "Curated", 0));
    assert(atomic_load(&c.bytes) > 0);
    puts("PASS unavailable GUID never produces a device revision; copy verifies bytes");
    source_filesystems_close(&c);
    identity = "zfs:1234567890123456789:3333333333333333333";
    struct stat st;
    assert(!fstat(c.sfd, &st));
    assert(!sql_exec(&c, "BEGIN IMMEDIATE"));
    int64_t rolled_back = source_filesystem(&c, c.sfd, NULL, &st);
    assert(rolled_back > 0);
    assert(!sql_rollback(&c));
    assert(!sql_exec(
        &c, "INSERT INTO source_filesystems(identity,name) VALUES('fixture:other','other')"));
    int64_t registered = source_filesystem(&c, c.sfd, NULL, &st);
    assert(registered > 0 && registered != rolled_back);
    assert(registered == source_filesystem(&c, c.sfd, NULL, &st));
    puts("PASS cached filesystem references survive transaction rollback safely");
    catalog_close(&c);
    return 0;
}
