#ifndef CCURATE_USAGE_INTERNAL_H
#define CCURATE_USAGE_INTERNAL_H

#include "usage.h"

typedef struct {
    Catalog catalog, cache;
    Catalog *owner;
    atomic_int stop, cancel, paused;
    atomic_ullong serial;
    uint64_t executing;
    pthread_mutex_t mutex;
    UsageStatus status;
    int64_t started;
    int64_t revision, full, pending, indexed_revision;
    int full_build;
    char token[64];
    struct UsagePageCache *pages;
} UsageDb;

int usage_tracking_open(UsageDb *db);
int usage_clock(UsageDb *db);
int usage_database_open(UsageDb *db);
int usage_index(UsageDb *db, int force);
UsageResult *usage_query(UsageDb *db, const UsageRequest *request);
void usage_phase(UsageDb *db, int phase, const char *message);
int usage_interrupted(void *arg);
int usage_query_interrupted(void *arg);
int usage_duplicate_build(UsageDb *db, int full);
int usage_rollup(UsageDb *db);
int usage_stmt_done(Catalog *catalog, sqlite3_stmt *stmt);
void usage_totals_read(sqlite3_stmt *stmt, int column, UsageTotals *totals);
void usage_row_free(UsageRow *row);
void usage_request_clear(UsageRequest *request);
void usage_request_copy(UsageRequest *to, const UsageRequest *from);
UsageResult *usage_cache_load(UsageDb *db, const UsageRequest *request);
void usage_cache_store(UsageDb *db, const UsageRequest *request, const UsageResult *result);
void usage_cache_free(UsageDb *db);

#endif
