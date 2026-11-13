#ifndef CCURATE_USAGE_H
#define CCURATE_USAGE_H

#include "curator.h"

/* Logical content sizes only. Unverified bytes are never included in deduplication. */
enum {
    USAGE_PAGE = 500,
    USAGE_TREE_LIMIT = 2000,
    USAGE_EXPAND_LIMIT = 24,
    USAGE_CHART_LIMIT = 160,
    USAGE_COLUMNS = 7
};
enum {
    USAGE_NAME,
    USAGE_BYTES,
    USAGE_SHARE,
    USAGE_UNIQUE,
    USAGE_REPEATED,
    USAGE_FILES,
    USAGE_UNKNOWN
};
enum { USAGE_IDLE, USAGE_BUILDING, USAGE_READY, USAGE_CANCELLED, USAGE_ERROR, USAGE_WAITING };
typedef struct {
    int64_t bytes, hashed, repeated, files, unknown_bytes, unknown_files, entries, errors;
} UsageTotals;
typedef struct {
    int64_t id;
    char *path, *name, *kind;
    UsageTotals totals;
    int depth, discarded, more;
} UsageRow;
typedef struct {
    UsageRow row;
    int parent; /* -1: current directory, otherwise an earlier chart node. */
    int other;
} UsageNode;
typedef struct {
    char *path, *selection;
    char *expanded[USAGE_EXPAND_LIMIT];
    int expanded_count, page, sort, descending, keep, depth, occurrence_page;
} UsageRequest;
typedef struct {
    uint64_t serial;
    int64_t revision;
    char *path, *details, *error;
    UsageTotals totals;
    UsageRow *rows;
    int count, page, complete;
    int64_t children;
    UsageNode nodes[USAGE_CHART_LIMIT];
    int node_count;
} UsageResult;
typedef struct {
    int phase;
    uint64_t done, total;
    int64_t revision, indexed_revision;
    double elapsed;
    char message[256];
} UsageStatus;
typedef struct UsageService UsageService;

UsageService *usage_start(Catalog *owner);
void usage_stop(UsageService *service);
uint64_t usage_submit(UsageService *service, const UsageRequest *request);
UsageResult *usage_take(UsageService *service);
void usage_status(UsageService *service, UsageStatus *status);
void usage_pause(UsageService *service, int pause);
void usage_cancel(UsageService *service);
void usage_rebuild(UsageService *service);
void usage_save(UsageService *service, const char *preferences);
void usage_result_free(UsageResult *result);

#endif
