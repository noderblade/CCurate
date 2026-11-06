#include "curator.h"
/* This test binary must execute its checks in Release builds as well. */
#undef NDEBUG
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
static atomic_int finished;
static int scan_rc;
static void *scan(void *p)
{
    scan_rc = scan_source(p, 0);
    atomic_store(&finished, 1);
    return NULL;
}
static void tick(void)
{
    struct timespec t = {0, 1000000};
    nanosleep(&t, NULL);
}
static ViewResult *receive(ViewService *s, uint64_t serial, unsigned flags)
{
    int64_t start = monotonic_ns();
    for (;;) {
        ViewResult *r = view_take(s);
        if (r) {
            if (r->serial == serial && (r->flags & flags) == flags)
                return r;
            view_result_free(r);
        }
        assert(monotonic_ns() - start < 30000000000LL);
        tick();
    }
}
int main(int argc, char **argv)
{
    if (argc != 4)
        return 2;
    Catalog c;
    assert(!catalog_open(&c, argv[2], argv[3], 0));
    if (!strcmp(argv[1], "live")) {
        Entry before = {0};
        assert(!load_entry(&c, "a-kept.txt", &before, 1));
        assert(before.accepted && !strcmp(before.state, "verified"));
        ViewService *service = view_start(&c);
        assert(service);
        pthread_t thread;
        assert(!pthread_create(&thread, NULL, scan, &c));
        ScanStatus status;
        int64_t start = monotonic_ns();
        do {
            tick();
            scan_status(&c, &status);
            assert(monotonic_ns() - start < 10000000000LL);
        } while (status.phase == SCAN_ENUMERATE && status.found < 512);
        assert(status.phase == SCAN_ENUMERATE);
        ViewRequest request = {.flags = VIEW_RIGHT,
                               .path = {"", ""},
                               .search = {"", ""},
                               .detail_path = "",
                               .fast = 1};
        uint64_t serial = view_submit(service, &request);
        ViewResult *r = receive(service, serial, VIEW_RIGHT);
        assert(r->count[1] > 1 && r->total[1] >= 127);
        Entry *live = r->rows[1];
        assert(!strcmp(live->name, "a-kept.txt") && live->pending && !live->accepted &&
               !*live->hash);
        assert(live->size == 26 && !strcmp(live->kind, "file"));
        scan_status(&c, &status);
        assert(status.phase == SCAN_ENUMERATE);
        printf("live enumeration PASS: %d visible rows, %llu discovered; pending metadata before "
               "hashing\n",
               r->count[1], (unsigned long long)status.found);
        view_result_free(r);
        atomic_store(&c.cancel, 1);
        pthread_join(thread, NULL);
        assert(scan_rc);
        view_stop(service);
        Entry history = {0}, pending = {0};
        assert(!load_entry(&c, "a-kept.txt", &history, 1));
        assert(history.version == before.version && history.accepted &&
               !strcmp(history.hash, before.hash));
        assert(!load_entry(&c, "a-kept.txt", &pending, 0) && pending.pending);
        assert(count_entries(&c, "", 1, 1, "a-kept", 1) == 0);
        assert(count_entries(&c, "", 0, 1, "a-kept", 1) == 1);
        atomic_store(&c.cancel, 0);
        assert(copy_entry(&c, "a-kept.txt", "Pending", 0));
        assert(copy_entry(&c, "", "Pending", 1));
        assert(!export_archive(&c));
        entry_free(&before);
        entry_free(&history);
        entry_free(&pending);
        puts("cancelled live scan / retained history / pending-copy refusal PASS");
    } else if (!strcmp(argv[1], "scan")) {
        pthread_t thread;
        assert(!pthread_create(&thread, NULL, scan, &c));
        int saw_hash = 0, saw_enum = 0;
        while (!atomic_load(&finished)) {
            ScanStatus s;
            scan_status(&c, &s);
            saw_enum |= s.phase == SCAN_ENUMERATE;
            saw_hash |= s.phase == SCAN_HASH;
            assert(s.fraction <= 1);
            assert(s.current_bytes <= s.planned_bytes || s.phase != SCAN_HASH);
            if (s.phase == SCAN_HASH && s.read_bytes) {
                printf("progress bytes=%llu done=%llu total=%llu fraction=%.3f eta=%.3f\n",
                       (unsigned long long)s.read_bytes, (unsigned long long)s.done,
                       (unsigned long long)s.found, s.fraction, s.eta);
            }
            struct timespec wait = {0, 20000000};
            nanosleep(&wait, NULL);
        }
        pthread_join(thread, NULL);
        assert(!scan_rc);
        ScanStatus s;
        scan_status(&c, &s);
        assert(s.phase == SCAN_DONE && s.done == s.found && s.resolved_bytes == s.planned_bytes);
        printf("scan PASS entries=%llu planned=%llu read=%llu saw_enum=%d saw_hash=%d\n",
               (unsigned long long)s.found, (unsigned long long)s.planned_bytes,
               (unsigned long long)s.read_bytes, saw_enum, saw_hash);
    } else {
        assert(!prepare_hash_groups(&c));
        ViewService *service = view_start(&c);
        assert(service);
        ViewRequest request = {.flags = VIEW_RIGHT,
                               .path = {"", "big"},
                               .search = {"", ""},
                               .detail_path = "",
                               .fast = 1};
        int64_t start = monotonic_ns();
        uint64_t serial = view_submit(service, &request);
        assert(monotonic_ns() - start < 100000000LL);
        ViewResult *r = receive(service, serial, VIEW_RIGHT);
        assert(r->count[1] == VIEW_PAGE_SIZE && r->total[1] >= 1000000);
        printf("first page %.3f s; total=%lld first=%s copies=%d\n", (monotonic_ns() - start) / 1e9,
               (long long)r->total[1], r->rows[1][0].name, r->rows[1][0].copies);
        assert(r->rows[1][0].copies >= 1000000);
        view_result_free(r);
        request.anchor[1] = "big/f0754321";
        request.page[1] = 0;
        start = monotonic_ns();
        serial = view_submit(service, &request);
        r = receive(service, serial, VIEW_RIGHT);
        assert(r->page[1] == 754 && !strcmp(r->rows[1][320].path, request.anchor[1]));
        printf("anchor in million-entry directory %.3f s; page=%d\n",
               (monotonic_ns() - start) / 1e9, r->page[1]);
        view_result_free(r);
        assert(!sql_exec(&c, "INSERT INTO entries(path,parent,name,kind) "
                             "VALUES('big/a-new','big','a-new','file')"));
        assert(entry_position(&c, "big", request.anchor[1], 0, 0, "", 1, 0) == 754321);
        assert(entry_position(&c, "big", request.anchor[1], 0, 0, "unmatched", 1, 0) == -1);
        assert(entry_position(&c, "big", "big/missing", 0, 0, "", 1, 0) == -1);
        assert(!sql_exec(&c, "DELETE FROM entries WHERE path='big/a-new'"));
        request.anchor[1] = NULL;
        request.page[1] = -1;
        start = monotonic_ns();
        serial = view_submit(service, &request);
        r = receive(service, serial, VIEW_RIGHT);
        assert(r->page[1] >= 999);
        printf("last page %.3f s; page=%d first=%s\n", (monotonic_ns() - start) / 1e9, r->page[1],
               r->rows[1][0].name);
        view_result_free(r);
        request.page[1] = 0;
        request.search[1] = "f1000000";
        serial = view_submit(service, &request);
        r = receive(service, serial, VIEW_RIGHT);
        assert(r->count[1] == 1 && r->total[1] == 1 && !strcmp(r->rows[1][0].name, "f1000000"));
        view_result_free(r);
        request.search[1] = "";
        assert(!sql_exec(&c, "UPDATE versions SET accepted=1,size=999 WHERE id=1000100;"
                             "UPDATE entries SET discarded=1 WHERE parent='big' AND id<=2100;"));
        request.historical = 1;
        request.anchor[1] = "big/f1000000";
        serial = view_submit(service, &request);
        r = receive(service, serial, VIEW_RIGHT);
        assert(r->count[1] == 1 && r->total[1] == 1 && !strcmp(r->rows[1][0].name, "f1000000"));
        view_result_free(r);
        request.historical = 0;
        request.unsecured = 1;
        request.anchor[1] = NULL;
        serial = view_submit(service, &request);
        r = receive(service, serial, VIEW_RIGHT);
        assert(r->count[1] == 1000 && r->total[1] == 998000 &&
               !strcmp(r->rows[1][0].name, "f0002001"));
        view_result_free(r);
        request.anchor[1] = "big/f0754321";
        serial = view_submit(service, &request);
        r = receive(service, serial, VIEW_RIGHT);
        assert(r->page[1] == 752 && !strcmp(r->rows[1][320].path, request.anchor[1]));
        view_result_free(r);
        request.unsecured = 0;
        request.sort[1] = SORT_SIZE | SORT_DESC;
        serial = view_submit(service, &request);
        r = receive(service, serial, VIEW_RIGHT);
        assert(r->page[1] == 754 && !strcmp(r->rows[1][321].path, request.anchor[1]));
        view_result_free(r);
        request.anchor[1] = NULL;
        serial = view_submit(service, &request);
        r = receive(service, serial, VIEW_RIGHT);
        assert(r->count[1] == 1000 && !strcmp(r->rows[1][0].name, "f1000000"));
        view_result_free(r);
        puts("global name / history / unsecured filters and size sort PASS");
        struct stat st;
        assert(!fstatat(c.mfd, "archive.jsonl", &st, 0));
        start = monotonic_ns();
        while (atomic_load(&c.storage.jsonl) != (uint64_t)st.st_size) {
            assert(monotonic_ns() - start < 3000000000LL);
            tick();
        }
        assert(atomic_load(&c.storage.database) > 0);
        puts("background storage metrics PASS");
        request.flags = VIEW_BALANCE;
        view_submit(service, &request);
        tick();
        request.flags = VIEW_RIGHT;
        request.path[1] = "empty";
        request.page[1] = 0;
        start = monotonic_ns();
        serial = view_submit(service, &request);
        r = receive(service, serial, VIEW_RIGHT);
        assert(r->count[1] == 0);
        printf("superseded query %.3f s; latest view correct\n", (monotonic_ns() - start) / 1e9);
        view_result_free(r);
        view_save(service, "kept-left", "kept-right", 17, 2.75, 1, 3, .4, .6, .55, 1, 0, -1, .25,
                  "", "", "", 0, SORT_NAME, SORT_SIZE | SORT_DESC, 0, 0);
        view_stop(service);
        char *value = setting(&c, "ui.scale", "");
        assert(!strcmp(value, "2.750"));
        free(value);
        puts("async view / paging / cancellation / persistence PASS");
    }
    catalog_close(&c);
    return 0;
}
