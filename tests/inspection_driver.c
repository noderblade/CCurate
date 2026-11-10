#include "curator.h"
#undef NDEBUG
#include <assert.h>
#include <stdlib.h>
#include <time.h>

int main(int argc, char **argv)
{
    assert(argc == 4);
    Catalog c;
    assert(!catalog_open(&c, NULL, argv[1], 1));
    ViewService *service = view_start(&c);
    assert(service);
    ViewRequest request = {.flags = VIEW_DETAILS,
                           .path = {"", ""},
                           .search = {"", ""},
                           .detail_path = argv[2],
                           .historical = 1,
                           .occurrence_page = atoi(argv[3])};
    /* Replace an in-flight source request with destination inspection, including
     * when the right browser is in history mode and the source is disconnected. */
    view_submit(service, &request);
    request.detail_destination = 1;
    uint64_t serial = view_submit(service, &request);
    int64_t start = monotonic_ns();
    for (;;) {
        ViewResult *result = view_take(service);
        if (result && result->serial == serial && (result->flags & VIEW_DETAILS)) {
            assert(result->details);
            fputs(result->details, stdout);
            view_result_free(result);
            break;
        }
        view_result_free(result);
        assert(monotonic_ns() - start < 10000000000LL);
        struct timespec wait = {0, 1000000};
        nanosleep(&wait, NULL);
    }
    view_stop(service);
    catalog_close(&c);
    return 0;
}
