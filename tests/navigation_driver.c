#include "curator.h"
#undef NDEBUG
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static ViewResult *receive(ViewService *service, uint64_t serial)
{
    int64_t start = monotonic_ns();
    for (;;) {
        ViewResult *r = view_take(service);
        if (r && r->serial == serial && (r->flags & VIEW_RIGHT))
            return r;
        view_result_free(r);
        assert(monotonic_ns() - start < 180000000000LL);
        struct timespec wait = {0, 1000000};
        nanosleep(&wait, NULL);
    }
}

int main(int argc, char **argv)
{
    assert(argc == 3);
    Catalog c;
    assert(!catalog_open(&c, argv[1], argv[2], 0));
    ViewService *service = view_start(&c);
    assert(service);
    ViewRequest request = {
        .flags = VIEW_RIGHT, .path = {"", ""}, .search = {"", ""}, .detail_path = ""};
    const char *paths[] = {"", "group-00", "", "group-01", ""};
    for (unsigned i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        request.path[1] = (char *)paths[i];
        request.anchor[1] = i && !*paths[i] ? (char *)paths[i - 1] : NULL;
        int64_t start = monotonic_ns();
        ViewResult *r = receive(service, view_submit(service, &request));
        assert(r->count[1] > 0);
        printf("LIST %s %.6f seconds rows=%d first=%s children=%lld\n", *paths[i] ? paths[i] : "/",
               (monotonic_ns() - start) / 1e9, r->count[1], r->rows[1][0].name,
               (long long)r->rows[1][0].children);
        fflush(stdout);
        view_result_free(r);
    }
    view_stop(service);
    catalog_close(&c);
    return 0;
}
