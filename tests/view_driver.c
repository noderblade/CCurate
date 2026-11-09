#include "curator.h"
#undef NDEBUG
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int budget(void *data)
{
    int *calls = data;
    return ++*calls > 500;
}
static int saw_summary;
static ViewResult *receive(ViewService *service, uint64_t serial)
{
    int64_t start = monotonic_ns();
    for (;;) {
        ViewResult *r = view_take(service);
        if (r && (r->flags & VIEW_SUMMARY)) {
            assert(r->summary.valid);
            assert(r->summary.source_files == 12000);
            assert(r->summary.unique == 1 && r->summary.unique_bytes == 4096);
            saw_summary = 1;
        }
        if (r && r->serial == serial && (r->flags & VIEW_RIGHT))
            return r;
        view_result_free(r);
        assert(monotonic_ns() - start < 10000000000LL);
        struct timespec wait = {0, 1000000};
        nanosleep(&wait, NULL);
    }
}
static ViewResult *list(ViewService *service, ViewRequest *request)
{
    return receive(service, view_submit(service, request));
}
int main(int argc, char **argv)
{
    assert(argc == 3);
    Catalog c;
    assert(!catalog_open(&c, argv[1], argv[2], 0));
    int calls = 0;
    sqlite3_progress_handler(c.db, 100, budget, &calls);
    Entry *rows = NULL;
    int count = list_entries_page_deferred(&c, "", &rows, 0, 0, "", 0, 1000, 0, 0, NULL);
    sqlite3_progress_handler(c.db, 0, NULL, NULL);
    assert(count == 8 && calls < 500);
    for (int i = 0; i < count; i++)
        assert(rows[i].children == -1);
    entries_free(rows, count);
    puts("PASS bounded directory listing without descendant aggregation");

    ViewService *service = view_start(&c);
    assert(service);
    ViewRequest request = {
        .flags = VIEW_RIGHT, .path = {"", ""}, .search = {"", ""}, .detail_path = ""};
    ViewResult *r = list(service, &request);
    assert(r->count[1] == 8 && !strcmp(r->rows[1][0].name, "group-00"));
    view_result_free(r);
    request.path[1] = "group-00";
    r = list(service, &request);
    assert(r->count[1] == 1);
    view_result_free(r);
    request.path[1] = "";
    request.anchor[1] = "group-03";
    r = list(service, &request);
    assert(r->count[1] == 8 && !strcmp(r->rows[1][3].path, "group-03"));
    view_result_free(r);
    assert(!sql_exec(&c, "UPDATE entries SET reviewed=1,discarded=1 WHERE path='group-00'"));
    r = list(service, &request);
    assert(r->rows[1][0].reviewed && r->rows[1][0].discarded);
    view_result_free(r);
    request.anchor[1] = NULL;
    request.unsecured = 1;
    r = list(service, &request);
    assert(r->count[1] == 7 && !strcmp(r->rows[1][0].name, "group-01"));
    view_result_free(r);
    request.unsecured = 0;
    request.search[1] = "03";
    r = list(service, &request);
    assert(r->count[1] == 1 && !strcmp(r->rows[1][0].name, "group-03"));
    view_result_free(r);
    request.search[1] = "";
    request.sort[1] = SORT_DESC;
    r = list(service, &request);
    assert(r->count[1] == 8 && !strcmp(r->rows[1][0].name, "group-07"));
    view_result_free(r);
    request.sort[1] = SORT_NAME;
    request.historical = 1;
    r = list(service, &request);
    assert(r->count[1] == 0);
    view_result_free(r);
    request.historical = 0;
    puts("PASS page/anchor reuse, committed-change invalidation and independent view keys");

    request.flags = VIEW_BALANCE;
    request.refresh_summary = 1;
    view_submit(service, &request);
    request.refresh_summary = 0;
    request.flags = VIEW_RIGHT;
    for (int i = 0; i < 60; i++) {
        request.path[1] = i % 2 ? "group-00" : "";
        r = list(service, &request);
        view_result_free(r);
    }
    int64_t start = monotonic_ns();
    while (!saw_summary) {
        r = view_take(service);
        if (r && (r->flags & VIEW_SUMMARY)) {
            assert(r->summary.valid && r->summary.unique_bytes == 4096);
            saw_summary = 1;
        }
        view_result_free(r);
        assert(monotonic_ns() - start < 10000000000LL);
        struct timespec wait = {0, 1000000};
        nanosleep(&wait, NULL);
    }
    puts("PASS statistics complete independently of repeated navigation requests");
    /* Deferred counters must eventually replace the placeholder. */
    request.path[1] = "";
    uint64_t serial = view_submit(service, &request);
    start = monotonic_ns();
    for (;;) {
        r = receive(service, serial);
        int ready = r->rows[1][1].children == 1501;
        view_result_free(r);
        if (ready)
            break;
        assert(monotonic_ns() - start < 10000000000LL);
    }
    puts("PASS deferred subtree counters finish without delaying the initial list");
    view_stop(service);
    catalog_close(&c);
    return 0;
}
