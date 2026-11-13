#include "curator.h"
#include "usage.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifndef _WIN32
#include <sys/resource.h>
#endif

int main(int argc, char **argv)
{
    if (argc < 2)
        return 2;
    Catalog catalog;
    if (catalog_open(&catalog, NULL, argv[1], 1)) {
        fprintf(stderr, "%s\n", catalog.error);
        catalog_close(&catalog);
        return 1;
    }
    UsageService *service = usage_start(&catalog);
    if (!service) {
        catalog_close(&catalog);
        return 1;
    }
    UsageRequest request = {0};
    request.path = argc > 2 ? argv[2] : "";
    request.selection = argc > 4 ? argv[4] : "";
    request.keep = argc > 3 ? atoi(argv[3]) : 0;
    request.sort = USAGE_BYTES;
    request.descending = 1;
    request.depth = 5;
    uint64_t serial = usage_submit(service, &request);
    usage_pause(service, 0);
    UsageResult *result = NULL;
    UsageStatus status = {0};
    const char *interrupt = getenv("CCURATE_USAGE_INTERRUPT");
    int interrupt_stage = 0;
    int64_t started = monotonic_ns();
    while ((monotonic_ns() - started) / 1e9 < 900) {
        result = usage_take(service);
        if (result && result->serial == serial)
            break;
        usage_result_free(result);
        result = NULL;
        usage_status(service, &status);
        if (interrupt && interrupt_stage == 0 && status.phase == USAGE_BUILDING &&
            status.done >= 20000) {
            if (!strcmp(interrupt, "pause"))
                usage_pause(service, 1);
            else
                usage_cancel(service);
            interrupt_stage = 1;
        } else if (interrupt && interrupt_stage == 1 &&
                   (status.phase == USAGE_CANCELLED || status.phase == USAGE_WAITING)) {
            if (!strcmp(interrupt, "pause"))
                usage_pause(service, 0);
            else
                usage_rebuild(service);
            interrupt_stage = 2;
            fprintf(stderr, "Interrupted analysis resumed: %s\n", interrupt);
        }
        if (status.phase == USAGE_ERROR) {
            fprintf(stderr, "%s\n", status.message);
            break;
        }
        struct timespec delay = {0, 10000000};
        nanosleep(&delay, NULL);
    }
    int failed = !result || result->error || (interrupt && interrupt_stage != 2);
    fprintf(stderr, "Initial view: %.3f s\n", (monotonic_ns() - started) / 1e9);
    if (result && result->error)
        fprintf(stderr, "%s\n", result->error);
    if (!failed) {
        printf("{\"revision\":%lld,\"bytes\":%lld,\"unique\":%lld,\"repeated\":%lld,\"files\":%lld,"
               "\"unknown\":%lld,\"unknown_files\":%lld,\"children\":%lld,\"rows\":[",
               (long long)result->revision, (long long)result->totals.bytes,
               (long long)(result->totals.hashed - result->totals.repeated),
               (long long)result->totals.repeated, (long long)result->totals.files,
               (long long)result->totals.unknown_bytes, (long long)result->totals.unknown_files,
               (long long)result->children);
        for (int i = 0; i < result->count; i++) {
            UsageRow *r = result->rows + i;
            char *name = json_string(r->path);
            printf("%s{\"path\":%s,\"bytes\":%lld,\"unique\":%lld,\"repeated\":%lld}", i ? "," : "",
                   name, (long long)r->totals.bytes,
                   (long long)(r->totals.hashed - r->totals.repeated),
                   (long long)r->totals.repeated);
            free(name);
        }
        printf("],\"chart\":[");
        for (int i = 0; i < result->node_count; i++) {
            UsageNode *n = result->nodes + i;
            char *path = json_string(n->row.path);
            printf("%s{\"path\":%s,\"parent\":%d,\"bytes\":%lld,\"other\":%d}", i ? "," : "", path,
                   n->parent, (long long)n->row.totals.bytes, n->other);
            free(path);
        }
        char *detail = json_string(result->details ? result->details : "");
        printf("],\"details\":%s}\n", detail);
        free(detail);
    }
    usage_result_free(result);
    const char *repeat = getenv("CCURATE_USAGE_REPEAT");
    int repeats = repeat ? atoi(repeat) : 0;
    if (repeats > 1000)
        repeats = 1000;
    for (int i = 0; !failed && i < repeats; i++) {
        started = monotonic_ns();
        serial = usage_submit(service, &request);
        do {
            result = usage_take(service);
            if (result && result->serial == serial)
                break;
            usage_result_free(result);
            result = NULL;
            struct timespec delay = {0, 1000000};
            nanosleep(&delay, NULL);
        } while ((monotonic_ns() - started) / 1e9 < 60);
        failed = !result || result->error;
        fprintf(stderr, "Cached view %d: %.3f ms\n", i + 1, (monotonic_ns() - started) / 1e6);
        usage_result_free(result);
    }
    usage_stop(service);
    catalog_close(&catalog);
#ifndef _WIN32
    if (repeats) {
        struct rusage resources;
        if (!getrusage(RUSAGE_SELF, &resources))
            fprintf(stderr, "Peak RSS: %ld KiB\n", resources.ru_maxrss);
    }
#endif
    return failed;
}
