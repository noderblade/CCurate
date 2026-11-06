#include "curator.h"
#undef NDEBUG
#include <assert.h>
#include <stdlib.h>
#include <time.h>

int main(int argc, char **argv)
{
    if (argc != 11)
        return 2;
    Catalog c;
    assert(!catalog_open(&c, argv[1], argv[2], 0));
    assert(!prepare_hash_groups(&c));
    ViewService *service = view_start(&c);
    assert(service);
    int side = atoi(argv[3]);
    ViewRequest request = {.flags = side ? VIEW_RIGHT : VIEW_LEFT,
                           .path = {"", ""},
                           .search = {"", ""},
                           .detail_path = "",
                           .historical = atoi(argv[5]),
                           .fast = atoi(argv[10])};
    request.sort[side] = atoi(argv[4]);
    request.page[side] = atoi(argv[6]);
    request.search[side] = argv[7];
    request.anchor[side] = argv[8];
    request.unsecured = atoi(argv[9]);
    uint64_t serial = view_submit(service, &request);
    int64_t start = monotonic_ns();
    ViewResult *r = NULL;
    for (;;) {
        r = view_take(service);
        if (r && r->serial == serial && r->flags == request.flags)
            break;
        view_result_free(r);
        assert(monotonic_ns() - start < 30000000000LL);
        struct timespec wait = {0, 1000000};
        nanosleep(&wait, NULL);
    }
    printf("{\"page\":%d,\"total\":%lld,\"rows\":[", r->page[side], (long long)r->total[side]);
    for (int i = 0; i < r->count[side]; i++) {
        Entry *e = &r->rows[side][i];
        char *name = json_string(e->name), *state = json_string(e->state),
             *error = json_string(e->error);
        printf("%s{\"name\":%s,\"kind\":\"%s\",\"size\":%lld,\"mtime\":%lld,\"btime\":%lld,"
               "\"mode\":%lld,"
               "\"state\":%s,\"error\":%s,\"copies\":%d,\"accepted\":%d,\"rejected\":%d,"
               "\"pending\":%d,\"hash\":\"%s\"}",
               i ? "," : "", name, e->kind, (long long)e->size, (long long)e->mtime,
               (long long)e->btime, (long long)e->mode, state, error, e->copies, e->accepted,
               e->discarded, e->pending, e->hash);
        free(name);
        free(state);
        free(error);
    }
    puts("]}");
    view_result_free(r);
    view_stop(service);
    catalog_close(&c);
    return 0;
}
