#include "usage_internal.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct UsageService {
    UsageDb db;
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    UsageRequest last;
    UsageResult *ready;
    int wanted, force, initialized, have_request;
    char *preferences;
};
void usage_phase(UsageDb *db, int phase, const char *message)
{
    pthread_mutex_lock(&db->mutex);
    db->status.phase = phase;
    db->status.done = 0;
    db->status.total = 0;
    snprintf(db->status.message, sizeof(db->status.message), "%s", message);
    pthread_mutex_unlock(&db->mutex);
}
int usage_interrupted(void *arg)
{
    UsageDb *db = arg;
    return atomic_load(&db->stop) || atomic_load(&db->cancel) || atomic_load(&db->paused);
}
int usage_query_interrupted(void *arg)
{
    UsageDb *db = arg;
    return usage_interrupted(db) || atomic_load(&db->serial) != db->executing;
}
static void wait_tick(UsageService *s, int idle)
{
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_nsec += 250000000;
    if (until.tv_nsec >= 1000000000) {
        until.tv_nsec -= 1000000000;
        until.tv_sec++;
    }
    pthread_mutex_lock(&s->mutex);
    if (!atomic_load(&s->db.stop) && !(idle && (s->wanted || s->force)))
        pthread_cond_timedwait(&s->cond, &s->mutex, &until);
    pthread_mutex_unlock(&s->mutex);
}
static void save_pending(UsageService *s)
{
    if (!s->db.catalog.db)
        return;
    pthread_mutex_lock(&s->mutex);
    char *value = s->preferences;
    s->preferences = NULL;
    pthread_mutex_unlock(&s->mutex);
    if (value && set_setting(&s->db.catalog, "ui.usage", value)) {
        pthread_mutex_lock(&s->mutex);
        if (!s->preferences) {
            s->preferences = value;
            value = NULL;
        }
        pthread_mutex_unlock(&s->mutex);
    }
    free(value);
}
static void close_databases(UsageService *s)
{
    usage_cache_free(&s->db);
    if (s->db.cache.db) {
        sqlite3_progress_handler(s->db.cache.db, 0, NULL, NULL);
        sql_rollback(&s->db.cache);
        sqlite3_wal_checkpoint_v2(s->db.cache.db, NULL, SQLITE_CHECKPOINT_PASSIVE, NULL, NULL);
        sqlite3_close(s->db.cache.db);
        s->db.cache.db = NULL;
    }
    if (s->db.catalog.db) {
        sql_rollback(&s->db.catalog);
        sqlite3_close(s->db.catalog.db);
        s->db.catalog.db = NULL;
    }
}
static void *run(void *arg)
{
    UsageService *s = arg;
    UsageDb *db = &s->db;
    int blocked = 0;
    while (!atomic_load(&db->stop)) {
        save_pending(s);
        if (atomic_load(&db->paused)) {
            usage_phase(db, USAGE_WAITING, "Waiting for source catalog updates to finish");
            wait_tick(s, 0);
            continue;
        }
        if (atomic_load(&db->cancel)) {
            usage_phase(db, USAGE_CANCELLED, "Analysis cancelled; completed index remains saved");
            wait_tick(s, 0);
            continue;
        }
        pthread_mutex_lock(&s->mutex);
        int force = s->force;
        s->force = 0;
        pthread_mutex_unlock(&s->mutex);
        if (force)
            blocked = 0;
        if (blocked) {
            wait_tick(s, 0);
            continue;
        }
        if (!s->initialized) {
            usage_phase(db, USAGE_BUILDING, "Opening logical usage index");
            int rc = usage_tracking_open(db);
            if (db->catalog.db)
                sqlite3_progress_handler(db->catalog.db, 2000, usage_interrupted, db);
            if (!rc)
                rc = prepare_hash_groups(&db->catalog);
            if (db->catalog.db)
                sqlite3_progress_handler(db->catalog.db, 0, NULL, NULL);
            if (!rc)
                rc = usage_clock(db);
            if (!rc)
                rc = usage_database_open(db);
            if (rc) {
                int code = db->catalog.db ? sqlite3_errcode(db->catalog.db) : SQLITE_ERROR;
                if (usage_interrupted(db) || code == SQLITE_BUSY || code == SQLITE_LOCKED) {
                    close_databases(s);
                    wait_tick(s, 0);
                    continue;
                }
                char message[256];
                snprintf(message, sizeof(message), "%.250s",
                         *db->cache.error ? db->cache.error : db->catalog.error);
                usage_phase(db, USAGE_ERROR, message);
                close_databases(s);
                blocked = 1;
                continue;
            }
            s->initialized = 1;
        }
        if (usage_clock(db)) {
            usage_phase(db, USAGE_ERROR, db->catalog.error);
            wait_tick(s, 0);
            continue;
        }
        int changed = force || db->indexed_revision != db->revision;
        int build_result = changed ? usage_index(db, force) : 0;
        if (build_result) {
            if (!usage_interrupted(db)) {
                if (build_result == -2 || build_result == -3) {
                    usage_phase(
                        db, USAGE_WAITING,
                        "Catalog changed during analysis; retrying a consistent generation");
                } else {
                    usage_phase(db, USAGE_ERROR,
                                *db->cache.error
                                    ? db->cache.error
                                    : "Could not build the usage index; use Rebuild to retry");
                    blocked = 1;
                    activity_log(db->owner, LOG_ERROR, "usage.index", NULL, NULL, "%s",
                                 db->status.message);
                }
            }
            wait_tick(s, 0);
            continue;
        }
        pthread_mutex_lock(&db->mutex);
        db->status.indexed_revision = db->indexed_revision;
        db->status.revision = db->revision;
        if (db->status.phase != USAGE_READY) {
            db->status.phase = USAGE_READY;
            snprintf(db->status.message, sizeof(db->status.message), "Logical usage index ready");
        }
        pthread_mutex_unlock(&db->mutex);
        pthread_mutex_lock(&s->mutex);
        int wanted = s->have_request && (s->wanted || changed);
        UsageRequest request = {0};
        if (wanted) {
            usage_request_copy(&request, &s->last);
            s->wanted = 0;
            db->executing = atomic_load(&db->serial);
        }
        pthread_mutex_unlock(&s->mutex);
        if (wanted) {
            UsageResult *result = usage_query(db, &request);
            usage_request_clear(&request);
            if (!result && !usage_query_interrupted(db)) {
                /* A request can race a catalog writer; preserve the last complete view. */
                if (usage_clock(db) == 0 && db->revision != db->indexed_revision) {
                    pthread_mutex_lock(&s->mutex);
                    s->wanted = 1;
                    pthread_mutex_unlock(&s->mutex);
                } else {
                    result = xmalloc(sizeof(*result));
                    result->serial = db->executing;
                    result->error =
                        xstrdup(*db->cache.error ? db->cache.error
                                                 : "Directory is unavailable in the current "
                                                   "catalog. Use Root to return.");
                }
            }
            pthread_mutex_lock(&s->mutex);
            if (result && !usage_query_interrupted(db)) {
                usage_result_free(s->ready);
                s->ready = result;
                result = NULL;
            }
            pthread_mutex_unlock(&s->mutex);
            usage_result_free(result);
        }
        wait_tick(s, 1);
    }
    save_pending(s);
    close_databases(s);
    return NULL;
}
UsageService *usage_start(Catalog *owner)
{
    UsageService *s = xmalloc(sizeof(*s));
    s->db.owner = owner;
    s->db.catalog.log = owner->log;
    s->db.cache.log = owner->log;
    atomic_store(&s->db.paused, 1);
    pthread_mutex_init(&s->mutex, NULL);
    pthread_cond_init(&s->cond, NULL);
    pthread_mutex_init(&s->db.mutex, NULL);
    pthread_mutex_init(&s->db.catalog.progress_mutex, NULL);
    pthread_mutex_init(&s->db.cache.progress_mutex, NULL);
    if (pthread_create(&s->thread, NULL, run, s)) {
        pthread_mutex_destroy(&s->db.cache.progress_mutex);
        pthread_mutex_destroy(&s->db.catalog.progress_mutex);
        pthread_mutex_destroy(&s->db.mutex);
        pthread_mutex_destroy(&s->mutex);
        pthread_cond_destroy(&s->cond);
        free(s);
        return NULL;
    }
    return s;
}
void usage_stop(UsageService *s)
{
    if (!s)
        return;
    atomic_store(&s->db.stop, 1);
    pthread_mutex_lock(&s->mutex);
    pthread_cond_signal(&s->cond);
    pthread_mutex_unlock(&s->mutex);
    pthread_join(s->thread, NULL);
    usage_request_clear(&s->last);
    usage_result_free(s->ready);
    free(s->preferences);
    pthread_mutex_destroy(&s->db.cache.progress_mutex);
    pthread_mutex_destroy(&s->db.catalog.progress_mutex);
    pthread_mutex_destroy(&s->db.mutex);
    pthread_mutex_destroy(&s->mutex);
    pthread_cond_destroy(&s->cond);
    free(s);
}
uint64_t usage_submit(UsageService *s, const UsageRequest *r)
{
    if (!s)
        return 0;
    pthread_mutex_lock(&s->mutex);
    usage_request_clear(&s->last);
    usage_request_copy(&s->last, r);
    uint64_t serial = atomic_fetch_add(&s->db.serial, 1) + 1;
    s->wanted = s->have_request = 1;
    pthread_cond_signal(&s->cond);
    pthread_mutex_unlock(&s->mutex);
    return serial;
}
UsageResult *usage_take(UsageService *s)
{
    if (!s)
        return NULL;
    pthread_mutex_lock(&s->mutex);
    UsageResult *r = s->ready;
    s->ready = NULL;
    pthread_mutex_unlock(&s->mutex);
    return r;
}
void usage_status(UsageService *s, UsageStatus *status)
{
    memset(status, 0, sizeof(*status));
    if (!s)
        return;
    pthread_mutex_lock(&s->db.mutex);
    *status = s->db.status;
    if (s->db.started)
        status->elapsed = (monotonic_ns() - s->db.started) / 1e9;
    pthread_mutex_unlock(&s->db.mutex);
}
void usage_pause(UsageService *s, int pause)
{
    if (!s)
        return;
    int before = atomic_exchange(&s->db.paused, !!pause);
    if (before != !!pause) {
        pthread_mutex_lock(&s->mutex);
        s->wanted = s->have_request;
        pthread_cond_signal(&s->cond);
        pthread_mutex_unlock(&s->mutex);
    }
}
void usage_cancel(UsageService *s)
{
    if (s)
        atomic_store(&s->db.cancel, 1);
}
void usage_rebuild(UsageService *s)
{
    if (!s)
        return;
    atomic_store(&s->db.cancel, 0);
    pthread_mutex_lock(&s->mutex);
    s->force = s->wanted = 1;
    pthread_cond_signal(&s->cond);
    pthread_mutex_unlock(&s->mutex);
}
void usage_save(UsageService *s, const char *preferences)
{
    if (!s)
        return;
    pthread_mutex_lock(&s->mutex);
    free(s->preferences);
    s->preferences = xstrdup(preferences);
    pthread_cond_signal(&s->cond);
    pthread_mutex_unlock(&s->mutex);
}
