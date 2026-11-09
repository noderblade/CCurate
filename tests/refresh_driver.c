#include "source_refresh.h"
#undef NDEBUG
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static atomic_int done;
static void *interrupt_hash(void *data)
{
    Catalog *c = data;
    while (!atomic_load(&done) && atomic_load(&c->bytes) < 1024 * 1024)
        usleep(1000);
    atomic_store(&c->cancel, 1);
    return NULL;
}

static int data_version(sqlite3 *db)
{
    sqlite3_stmt *s;
    assert(sqlite3_prepare_v2(db, "PRAGMA data_version", -1, &s, NULL) == SQLITE_OK);
    assert(sqlite3_step(s) == SQLITE_ROW);
    int value = sqlite3_column_int(s, 0);
    sqlite3_finalize(s);
    return value;
}

int main(int argc, char **argv)
{
    assert(argc == 5);
    Catalog c;
    assert(!catalog_open(&c, argv[1], argv[2], 0));
    int result = 0;
    if (!strcmp(argv[3], "initial")) {
        printf("initial=%d\n", source_needs_initial_scan(&c));
    } else if (!strcmp(argv[3], "copy")) {
        result = refresh_copy_source(&c, argv[4]);
        if (!result)
            result = copy_entry(&c, argv[4], "Curated", 1);
    } else if (!strcmp(argv[3], "cancel")) {
        pthread_t worker;
        assert(!pthread_create(&worker, NULL, interrupt_hash, &c));
        result = refresh_directory(&c, argv[4]);
        atomic_store(&done, 1);
        pthread_join(worker, NULL);
        assert(result && atomic_load(&c.bytes) >= 1024 * 1024);
        assert(sqlite3_get_autocommit(c.db));
        result = 0;
    } else if (!strcmp(argv[3], "unchanged")) {
        sqlite3 *reader = NULL;
        char *name = join_path(c.state_dir, "catalog.sqlite");
        assert(sqlite3_open_v2(name, &reader, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK);
        free(name);
        int before = data_version(reader);
        for (int i = 0; i < 3; i++) {
            assert(!refresh_directory(&c, argv[4]));
            assert(!atomic_load(&c.bytes));
            assert(!atomic_load(&c.refresh_changed));
            assert(before == data_version(reader));
        }
        sqlite3_close(reader);
    } else
        abort();
    if (result)
        fprintf(stderr, "%s\n", c.error);
    catalog_close(&c);
    return result ? 1 : 0;
}
