/* All catalog queries, directory IO and UI-setting writes are off the Raven thread.
 * Latest-request-wins cancellation prevents stale results replacing a newer view. */
#include "curator.h"
#include "view_cache.h"
#include <dirent.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/statvfs.h>
#endif
#include <time.h>
#include <unistd.h>

enum { VIEW_SETTING_COUNT = 23 };

struct ViewService {
    Catalog reader;
    Catalog statistics_reader;
    pthread_t thread;
    pthread_t metrics_thread, statistics_thread;
    Catalog *owner;
    pthread_mutex_t mutex;
    pthread_cond_t cond, statistics_cond;
    atomic_int stop, running;
    atomic_ullong serial;
    uint64_t executing;
    ViewRequest *pending;
    ViewResult *ready;
    ViewResult *statistics_ready;
    unsigned statistics_wanted;
    int statistics_running;
    atomic_ullong statistics_serial;
    uint64_t statistics_executing;
    char *save[VIEW_SETTING_COUNT];
    int save_pending;
    char *target_cache;
    ViewCache *source_cache;
    Summary summary_cache;
    int64_t summary_version;
};
static void request_free(ViewRequest *r)
{
    if (!r)
        return;
    for (int i = 0; i < 2; i++) {
        free(r->path[i]);
        free(r->search[i]);
        free(r->anchor[i]);
    }
    free(r->detail_path);
    free(r);
}
void view_result_free(ViewResult *r)
{
    if (!r)
        return;
    for (int i = 0; i < 2; i++)
        entries_free(r->rows[i], r->count[i]);
    free(r->details);
    free(r->balance);
    entry_free(&r->cycle);
    free(r);
}
static int interrupted(void *arg)
{
    ViewService *s = arg;
    return atomic_load(&s->stop) || atomic_load(&s->serial) != s->executing;
}
static int statistics_interrupted(void *arg)
{
    ViewService *s = arg;
    return atomic_load(&s->stop) || atomic_load(&s->statistics_serial) != s->statistics_executing;
}
static int last_page(int64_t total)
{
    return total > 0 ? (int)((total - 1) / VIEW_PAGE_SIZE) : 0;
}
static void view_summary(ViewService *s, Summary *summary)
{
    Catalog *c = &s->statistics_reader;
    sqlite3_stmt *q = sql_prepare(c, "PRAGMA data_version");
    int64_t version = q && sqlite3_step(q) == SQLITE_ROW ? sqlite3_column_int64(q, 0) : -1;
    sqlite3_finalize(q);
    if (version >= 0 && s->summary_cache.valid && version == s->summary_version) {
        *summary = s->summary_cache;
        summary->free_bytes = atomic_load(&s->owner->storage.free_bytes);
        return;
    }
    catalog_summary(c, summary);
    q = sql_prepare(c, "PRAGMA data_version");
    int64_t after = q && sqlite3_step(q) == SQLITE_ROW ? sqlite3_column_int64(q, 0) : -1;
    sqlite3_finalize(q);
    if (summary->valid && version >= 0 && version == after) {
        s->summary_cache = *summary;
        s->summary_version = version;
    }
}
static char *target_order(int sort)
{
    const char *keys[] = {
        "t.name COLLATE BINARY",
        "CASE WHEN COALESCE(z.state,o.state)='corrupt' THEN 'CHANGED' WHEN "
        "COALESCE(z.state,o.state)='missing' THEN "
        "'MISSING' WHEN COALESCE(z.state,o.state)='unchecked' THEN 'UNCHECKED' WHEN "
        "COALESCE(z.state,o.state)='verified' THEN 'OK' WHEN t.name='" META_DIR
        "' THEN 'SESSION' ELSE '' END",
        "t.size",
        "t.mtime",
        "t.btime",
        "(t.mode & 4095)"};
    int column = sort & SORT_COLUMN_MASK;
    if (column < 0 || column >= SORT_COUNT)
        column = SORT_NAME;
    return fmt("(t.kind='directory') DESC,%s %s,t.name COLLATE BINARY ASC", keys[column],
               sort & SORT_DESC ? "DESC" : "ASC");
}
static void target_rows(ViewService *s, ViewRequest *r, ViewResult *out)
{
    Catalog *c = &s->reader;
    if (!s->target_cache || strcmp(s->target_cache, r->path[0]) || r->target_refresh) {
        free(s->target_cache);
        s->target_cache = NULL;
        if (sql_exec(
                c, "DROP TABLE IF EXISTS temp.target_listing;CREATE TEMP TABLE target_listing("
                   "name TEXT PRIMARY KEY,kind TEXT,size INTEGER,mtime INTEGER,btime INTEGER,mode "
                   "INTEGER,uid INTEGER,gid INTEGER);"))
            return;
        int fd = open_beneath(c->dfd, r->path[0], O_RDONLY | O_DIRECTORY, 0);
        DIR *d = fd >= 0 ? fdopendir(fd) : NULL;
        if (!d) {
            if (fd >= 0)
                close(fd);
            return;
        }
        sqlite3_stmt *insert = sql_prepare(c, "INSERT INTO target_listing VALUES(?,?,?,?,?,?,?,?)");
        if (!insert || sql_exec(c, "BEGIN")) {
            sqlite3_finalize(insert);
            closedir(d);
            return;
        }
        struct dirent *de;
        int rc = 0;
        while (!interrupted(s) && (de = readdir(d))) {
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
                continue;
            struct stat st;
            if (fstatat(fd, de->d_name, &st, AT_SYMLINK_NOFOLLOW))
                continue;
            const char *kind = S_ISDIR(st.st_mode)   ? "directory"
                               : S_ISREG(st.st_mode) ? "file"
                               : S_ISLNK(st.st_mode) ? "symlink"
                                                     : "special";
            struct statx sx;
            int64_t born = 0;
            if (!statx(fd, de->d_name, AT_SYMLINK_NOFOLLOW, STATX_BTIME, &sx) &&
                (sx.stx_mask & STATX_BTIME))
                born = (int64_t)sx.stx_btime.tv_sec * 1000000000 + sx.stx_btime.tv_nsec;
            bind_text(insert, 1, de->d_name);
            bind_text(insert, 2, kind);
            sqlite3_bind_int64(insert, 3, st.st_size);
            sqlite3_bind_int64(insert, 4, mtime_ns(&st));
            sqlite3_bind_int64(insert, 5, born);
            sqlite3_bind_int64(insert, 6, st.st_mode);
            sqlite3_bind_int64(insert, 7, st.st_uid);
            sqlite3_bind_int64(insert, 8, st.st_gid);
            int step = sqlite3_step(insert);
            if (step != SQLITE_DONE) {
                rc = sql_error(c, step, "build destination listing");
                break;
            }
            sqlite3_reset(insert);
            sqlite3_clear_bindings(insert);
        }
        sqlite3_finalize(insert);
        closedir(d);
        if (sql_end(c, rc || interrupted(s) ? -1 : 0))
            return;
        if (sql_exec(c,
                     "CREATE INDEX target_order ON target_listing((kind='directory') DESC,name)"))
            return;
        s->target_cache = xstrdup(r->path[0]);
    }
    sqlite3_stmt *q = sql_prepare(
        c, "SELECT COUNT(*) FROM target_listing WHERE (?='' OR instr(lower(name),lower(?))>0)");
    bind_text(q, 1, r->search[0]);
    bind_text(q, 2, r->search[0]);
    if (sqlite3_step(q) == SQLITE_ROW)
        out->total[0] = sqlite3_column_int64(q, 0);
    sqlite3_finalize(q);
    int page = r->page[0], last = last_page(out->total[0]);
    char *prefix = *r->path[0] ? fmt("%s/", r->path[0]) : xstrdup("");
    char *order = target_order(r->sort[0]);
    const char *from =
        "FROM target_listing t LEFT JOIN objects o ON o.dest=(?||t.name) LEFT JOIN empty_copies z "
        "ON z.version=(SELECT MAX(version) FROM empty_copies WHERE dest=(?1||t.name)) WHERE "
        "(?='' OR instr(lower(t.name),lower(?))>0)";
    if (r->anchor[0] && *r->anchor[0]) {
        char *parent = parent_path(r->anchor[0]);
        if (!strcmp(parent, r->path[0])) {
            char *rank_sql = fmt("SELECT position FROM (SELECT t.name,ROW_NUMBER() OVER (ORDER BY "
                                 "%s)-1 AS position %s) WHERE name=?",
                                 order, from);
            q = sql_prepare(c, rank_sql);
            free(rank_sql);
            bind_text(q, 1, prefix);
            bind_text(q, 2, r->search[0]);
            bind_text(q, 3, r->search[0]);
            bind_text(q, 4, base_name(r->anchor[0]));
            if (sqlite3_step(q) == SQLITE_ROW)
                page = (int)(sqlite3_column_int64(q, 0) / VIEW_PAGE_SIZE);
            sqlite3_finalize(q);
        }
        free(parent);
    }
    if (page < 0 || page > last)
        page = last;
    out->page[0] = page;
    char *sql = fmt(
        "SELECT "
        "t.name,t.kind,t.size,t.mtime,t.btime,t.mode,t.uid,t.gid,COALESCE(z.state,o.state),o.hash "
        "%s ORDER BY %s LIMIT ? OFFSET ?",
        from, order);
    q = sql_prepare(c, sql);
    free(sql);
    free(order);
    bind_text(q, 1, prefix);
    bind_text(q, 2, r->search[0]);
    bind_text(q, 3, r->search[0]);
    sqlite3_bind_int(q, 4, VIEW_PAGE_SIZE);
    sqlite3_bind_int64(q, 5, (int64_t)page * VIEW_PAGE_SIZE);
    free(prefix);
    Entry *rows = xmalloc(VIEW_PAGE_SIZE * sizeof(Entry));
    int n = 0;
    while (!interrupted(s) && sqlite3_step(q) == SQLITE_ROW) {
        const char *name = (char *)sqlite3_column_text(q, 0);
        Entry *e = rows + n++;
        e->name = xstrdup(name);
        e->path = join_path(r->path[0], name);
        e->parent = xstrdup(r->path[0]);
        e->kind = xstrdup((char *)sqlite3_column_text(q, 1));
        e->size = sqlite3_column_int64(q, 2);
        e->mtime = sqlite3_column_int64(q, 3);
        e->btime = sqlite3_column_int64(q, 4);
        e->mode = sqlite3_column_int64(q, 5);
        e->uid = sqlite3_column_int64(q, 6);
        e->gid = sqlite3_column_int64(q, 7);
        e->state = xstrdup((const char *)sqlite3_column_text(q, 8));
        e->hash = xstrdup((const char *)sqlite3_column_text(q, 9));
        e->dest = xstrdup(e->path);
        e->error = xstrdup("");
    }
    sqlite3_finalize(q);
    out->rows[0] = rows;
    out->count[0] = n;
}
static ViewResult *query(ViewService *s, ViewRequest *r)
{
    ViewResult *out = xmalloc(sizeof(*out));
    out->serial = s->executing;
    out->flags = r->flags;
    Catalog *c = &s->reader;
    if (!r->fast && prepare_hash_groups(c))
        return out;
    if (r->flags & VIEW_LEFT)
        target_rows(s, r, out);
    if (!interrupted(s) && (r->flags & VIEW_RIGHT) && !view_cache_load(s->source_cache, r, out)) {
        out->total[1] =
            count_entries(c, r->path[1], r->historical, r->unsecured, r->search[1], r->fast);
        int page = r->page[1], last = last_page(out->total[1]);
        if (r->anchor[1] && *r->anchor[1]) {
            int64_t position = entry_position(c, r->path[1], r->anchor[1], r->historical,
                                              r->unsecured, r->search[1], r->fast, r->sort[1]);
            if (position >= 0)
                page = (int)(position / VIEW_PAGE_SIZE);
        }
        if (page < 0 || page > last)
            page = last;
        out->page[1] = page;
        if (!interrupted(s))
            out->count[1] = list_entries_page_deferred(c, r->path[1], out->rows + 1, r->historical,
                                                       r->unsecured, r->search[1], page,
                                                       VIEW_PAGE_SIZE, r->fast, r->sort[1], NULL);
        if (!interrupted(s))
            view_cache_store(s->source_cache, c, r, out);
    }
    if (r->flags & VIEW_RIGHT)
        r->page[1] = out->page[1];
    if (!interrupted(s) && (r->flags & VIEW_CYCLE) && *r->detail_path) {
        sqlite3_stmt *q = sql_prepare(
            c, "SELECT COALESCE(MAX(CASE WHEN v.id<? THEN v.id END),MAX(v.id)) "
               "FROM versions v JOIN entries e ON e.id=v.eid WHERE e.path=? AND v.accepted=1");
        sqlite3_bind_int64(q, 1, r->detail_version);
        bind_text(q, 2, r->detail_path);
        int64_t version = sqlite3_step(q) == SQLITE_ROW ? sqlite3_column_int64(q, 0) : 0;
        sqlite3_finalize(q);
        if (version)
            load_version(c, r->detail_path, version, &out->cycle);
    }
    if (!interrupted(s) && (r->flags & VIEW_DETAILS) && *r->detail_path) {
        Entry e = {0};
        int rc = r->historical && r->detail_version
                     ? load_version(c, r->detail_path, r->detail_version, &e)
                     : load_entry(c, r->detail_path, &e, r->historical);
        if (!rc) {
            out->details = entry_details_page(c, &e, r->occurrence_page);
            entry_free(&e);
        }
    }
    return out;
}
static int save_settings(ViewService *s, char **values)
{
    /* A busy data writer must not stall the renderer. Retry later in this worker. */
    if (sqlite3_exec(s->reader.db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK)
        return -1;
    const char *keys[] = {"ui.left",       "ui.right",        "ui.selected",     "ui.scale",
                          "ui.filter",     "ui.page",         "ui.split.top",    "ui.split.bottom",
                          "ui.split.rows", "ui.theme",        "ui.log.visible",  "ui.log.level",
                          "ui.split.log",  "ui.font.file",    "ui.font.family",  "ui.font.style",
                          "ui.font.index", "ui.sort.left",    "ui.sort.right",   "ui.date.left",
                          "ui.date.right", "ui.columns.left", "ui.columns.right"};
    _Static_assert(sizeof(keys) / sizeof(keys[0]) == VIEW_SETTING_COUNT, "UI settings count");
    int rc = 0;
    for (int i = 0; i < VIEW_SETTING_COUNT; i++)
        rc |= set_setting(&s->reader, keys[i], values[i]);
    rc = sql_end(&s->reader, rc);
    if (!rc)
        activity_log(&s->reader, LOG_DEBUG, "settings.saved", NULL, NULL,
                     "Pane locations, scale, font, theme and layout saved");
    return rc;
}
static void publish(ViewService *s, ViewResult *out)
{
    ViewResult *old = NULL;
    pthread_mutex_lock(&s->mutex);
    if (!interrupted(s)) {
        if (s->ready && s->ready->serial == out->serial) {
            ViewResult *dest = s->ready;
            dest->flags |= out->flags;
            for (int j = 0; j < 2; j++)
                if (out->flags & (j ? VIEW_RIGHT : VIEW_LEFT)) {
                    entries_free(dest->rows[j], dest->count[j]);
                    dest->rows[j] = out->rows[j];
                    dest->count[j] = out->count[j];
                    dest->page[j] = out->page[j];
                    dest->total[j] = out->total[j];
                    out->rows[j] = NULL;
                    out->count[j] = 0;
                }
            if (out->flags & VIEW_SUMMARY)
                dest->summary = out->summary;
            if (out->details) {
                dest->details = out->details;
                out->details = NULL;
            }
            if (out->balance) {
                dest->balance = out->balance;
                out->balance = NULL;
            }
            if (out->cycle.path) {
                dest->cycle = out->cycle;
                memset(&out->cycle, 0, sizeof(out->cycle));
            }
        } else {
            old = s->ready;
            s->ready = out;
            out = NULL;
        }
    }
    pthread_mutex_unlock(&s->mutex);
    view_result_free(old);
    view_result_free(out);
}
static void *run(void *arg)
{
    ViewService *s = arg;
    for (;;) {
        pthread_mutex_lock(&s->mutex);
        while (!s->pending && !s->save_pending && !atomic_load(&s->stop))
            pthread_cond_wait(&s->cond, &s->mutex);
        ViewRequest *r = s->pending;
        s->pending = NULL;
        s->executing = atomic_load(&s->serial);
        char *values[VIEW_SETTING_COUNT] = {0};
        int saving = s->save_pending;
        if (saving) {
            for (int i = 0; i < VIEW_SETTING_COUNT; i++) {
                values[i] = s->save[i];
                s->save[i] = NULL;
            }
            s->save_pending = 0;
        }
        atomic_store(&s->running, r || saving);
        pthread_mutex_unlock(&s->mutex);
        if (saving) {
            sqlite3_progress_handler(s->reader.db, 0, NULL, NULL);
            int rc = save_settings(s, values);
            pthread_mutex_lock(&s->mutex);
            if (rc && !s->save_pending && !atomic_load(&s->stop)) {
                for (int i = 0; i < VIEW_SETTING_COUNT; i++) {
                    s->save[i] = values[i];
                    values[i] = NULL;
                }
                s->save_pending = 1;
            }
            pthread_mutex_unlock(&s->mutex);
            for (int i = 0; i < VIEW_SETTING_COUNT; i++)
                free(values[i]);
            if (rc && !r && !atomic_load(&s->stop)) {
                struct timespec wait = {0, 100000000};
                nanosleep(&wait, NULL);
            }
        }
        if (atomic_load(&s->stop)) {
            request_free(r);
            break;
        }
        if (r) {
            view_cache_refresh(s->source_cache, &s->reader);
            const unsigned order[] = {VIEW_LEFT, VIEW_RIGHT, VIEW_CYCLE, VIEW_DETAILS};
            unsigned wanted = r->flags;
            for (unsigned i = 0; i < sizeof(order) / sizeof(order[0]) && !interrupted(s); i++) {
                if (!(wanted & order[i]))
                    continue;
                sqlite3_progress_handler(s->reader.db, 1000, interrupted, s);
                r->flags = order[i];
                ViewResult *out = query(s, r);
                /* No request may hand a transaction/lock to the next request or
                 * to the settings writer. Cleanup is deliberately not cancellable. */
                sqlite3_progress_handler(s->reader.db, 0, NULL, NULL);
                sql_rollback(&s->reader);
                publish(s, out);
            }
            if (!interrupted(s) && !r->fast) {
                ViewResult *out = xmalloc(sizeof(*out));
                out->serial = s->executing;
                out->flags = VIEW_RIGHT;
                if (view_cache_load(s->source_cache, r, out)) {
                    int changed = 0;
                    sqlite3_progress_handler(s->reader.db, 1000, interrupted, s);
                    for (int j = 0; j < out->count[1] && !interrupted(s); j++) {
                        Entry *e = &out->rows[1][j];
                        if (strcmp(e->kind, "directory") || e->children >= 0)
                            continue;
                        if (entry_directory_totals(&s->reader, e))
                            break;
                        changed = 1;
                    }
                    sqlite3_progress_handler(s->reader.db, 0, NULL, NULL);
                    if (changed) {
                        /* Completed counters survive cancellation by a newer selection.
                         * A new SQLite data version still invalidates the whole page. */
                        view_cache_store(s->source_cache, &s->reader, r, out);
                        publish(s, out);
                        out = NULL;
                    }
                }
                view_result_free(out);
            }
            request_free(r);
        }
        atomic_store(&s->running, 0);
    }
    sqlite3_progress_handler(s->reader.db, 0, NULL, NULL);
    sql_rollback(&s->reader);
    atomic_store(&s->running, 0);
    return NULL;
}
static uint64_t file_size(int fd, const char *name)
{
    struct stat st;
    return !fstatat(fd, name, &st, AT_SYMLINK_NOFOLLOW) && S_ISREG(st.st_mode) ? st.st_size : 0;
}
static void *metrics(void *arg)
{
    ViewService *s = arg;
    Catalog *c = s->owner;
    while (!atomic_load(&s->stop)) {
        atomic_store(&c->storage.database, file_size(c->mfd, "catalog.sqlite"));
        atomic_store(&c->storage.wal, file_size(c->mfd, "catalog.sqlite-wal"));
        atomic_store(&c->storage.shm, file_size(c->mfd, "catalog.sqlite-shm"));
        atomic_store(&c->storage.jsonl, file_size(c->mfd, "archive.jsonl"));
        struct statvfs fs;
        if (!fstatvfs(c->dfd, &fs))
            atomic_store(&c->storage.free_bytes, (uint64_t)fs.f_bavail * fs.f_frsize);
        for (int i = 0; i < 10 && !atomic_load(&s->stop); i++) {
            struct timespec wait = {0, 100000000};
            nanosleep(&wait, NULL);
        }
    }
    return NULL;
}
static void *statistics(void *arg)
{
    ViewService *s = arg;
    for (;;) {
        pthread_mutex_lock(&s->mutex);
        while (!s->statistics_wanted && !atomic_load(&s->stop))
            pthread_cond_wait(&s->statistics_cond, &s->mutex);
        if (atomic_load(&s->stop)) {
            pthread_mutex_unlock(&s->mutex);
            break;
        }
        unsigned wanted = s->statistics_wanted;
        s->statistics_wanted = 0;
        s->statistics_running = !!wanted;
        s->statistics_executing = atomic_load(&s->statistics_serial);
        pthread_mutex_unlock(&s->mutex);
        if (wanted) {
            ViewResult *out = xmalloc(sizeof(*out));
            out->flags = VIEW_SUMMARY | VIEW_BALANCE;
            sqlite3_progress_handler(s->statistics_reader.db, 1000, statistics_interrupted, s);
            view_summary(s, &out->summary);
            sqlite3_progress_handler(s->statistics_reader.db, 0, NULL, NULL);
            sql_rollback(&s->statistics_reader);
            out->balance = summary_format(&out->summary);
            pthread_mutex_lock(&s->mutex);
            if (!statistics_interrupted(s)) {
                view_result_free(s->statistics_ready);
                s->statistics_ready = out;
                out = NULL;
            }
            s->statistics_running = 0;
            pthread_mutex_unlock(&s->mutex);
            view_result_free(out);
        }
    }
    return NULL;
}
ViewService *view_start(Catalog *c)
{
    ViewService *s = xmalloc(sizeof(*s));
    s->owner = c;
    s->reader.log = c->log;
    s->reader.source = c->source;
    s->reader.destination = c->destination;
    s->reader.state_dir = c->state_dir;
    s->reader.sfd = c->sfd;
    s->reader.dfd = c->dfd;
    pthread_mutex_init(&s->reader.progress_mutex, NULL);
    pthread_mutex_init(&s->mutex, NULL);
    pthread_cond_init(&s->cond, NULL);
    pthread_cond_init(&s->statistics_cond, NULL);
    char *path = join_path(c->state_dir, "catalog.sqlite");
    int rc =
        sqlite3_open_v2(path, &s->reader.db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX, NULL);
    free(path);
    if (rc != SQLITE_OK) {
        sqlite3_close(s->reader.db);
        free(s);
        return NULL;
    }
    sqlite3_busy_timeout(s->reader.db, 20);
    sql_exec(&s->reader, "PRAGMA temp_store=FILE; PRAGMA cache_size=-16384;");
    s->statistics_reader.dfd = c->dfd;
    s->statistics_reader.log = c->log;
    pthread_mutex_init(&s->statistics_reader.progress_mutex, NULL);
    path = join_path(c->state_dir, "catalog.sqlite");
    rc = sqlite3_open_v2(path, &s->statistics_reader.db,
                         SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, NULL);
    free(path);
    if (rc != SQLITE_OK) {
        sqlite3_close(s->statistics_reader.db);
        sqlite3_close(s->reader.db);
        pthread_mutex_destroy(&s->statistics_reader.progress_mutex);
        free(s);
        return NULL;
    }
    sqlite3_busy_timeout(s->statistics_reader.db, 20);
    sql_exec(&s->statistics_reader, "PRAGMA temp_store=FILE; PRAGMA cache_size=-16384;");
    s->source_cache = view_cache_create();
    if (pthread_create(&s->thread, NULL, run, s)) {
        view_cache_free(s->source_cache);
        sqlite3_close(s->statistics_reader.db);
        pthread_mutex_destroy(&s->statistics_reader.progress_mutex);
        sqlite3_close(s->reader.db);
        free(s);
        return NULL;
    }
    if (pthread_create(&s->metrics_thread, NULL, metrics, s)) {
        pthread_mutex_lock(&s->mutex);
        atomic_store(&s->stop, 1);
        pthread_cond_signal(&s->cond);
        pthread_mutex_unlock(&s->mutex);
        pthread_join(s->thread, NULL);
        view_cache_free(s->source_cache);
        sqlite3_close(s->statistics_reader.db);
        pthread_mutex_destroy(&s->statistics_reader.progress_mutex);
        sqlite3_close(s->reader.db);
        free(s);
        return NULL;
    }
    if (pthread_create(&s->statistics_thread, NULL, statistics, s)) {
        atomic_store(&s->stop, 1);
        pthread_mutex_lock(&s->mutex);
        pthread_cond_signal(&s->cond);
        pthread_mutex_unlock(&s->mutex);
        pthread_join(s->thread, NULL);
        pthread_join(s->metrics_thread, NULL);
        view_cache_free(s->source_cache);
        sqlite3_close(s->reader.db);
        sqlite3_close(s->statistics_reader.db);
        pthread_mutex_destroy(&s->reader.progress_mutex);
        pthread_mutex_destroy(&s->statistics_reader.progress_mutex);
        pthread_mutex_destroy(&s->mutex);
        pthread_cond_destroy(&s->cond);
        pthread_cond_destroy(&s->statistics_cond);
        free(s);
        return NULL;
    }
    return s;
}
uint64_t view_submit(ViewService *s, const ViewRequest *request)
{
    ViewRequest *r = xmalloc(sizeof(*r));
    *r = *request;
    for (int i = 0; i < 2; i++) {
        r->path[i] = xstrdup(request->path[i]);
        r->search[i] = xstrdup(request->search[i]);
        r->anchor[i] = xstrdup(request->anchor[i]);
    }
    r->detail_path = xstrdup(request->detail_path);
    pthread_mutex_lock(&s->mutex);
    unsigned statistics = r->flags & (VIEW_SUMMARY | VIEW_BALANCE);
    if (r->fast && (s->statistics_running || s->statistics_wanted)) {
        /* Do not pin a large read snapshot/WAL while a scan or copy is writing.
         * The GUI requests fresh totals again after the operation completes. */
        atomic_fetch_add(&s->statistics_serial, 1);
        s->statistics_wanted = 0;
    }
    if (statistics && !r->fast) {
        if (r->refresh_summary) {
            atomic_fetch_add(&s->statistics_serial, 1);
            view_result_free(s->statistics_ready);
            s->statistics_ready = NULL;
            s->statistics_wanted |= statistics;
        } else if (!s->statistics_running && !s->statistics_ready)
            s->statistics_wanted |= statistics;
    }
    if (s->statistics_wanted)
        pthread_cond_signal(&s->statistics_cond);
    r->flags &= ~(VIEW_SUMMARY | VIEW_BALANCE);
    request_free(s->pending);
    s->pending = r;
    uint64_t serial = atomic_fetch_add(&s->serial, 1) + 1;
    pthread_cond_signal(&s->cond);
    pthread_mutex_unlock(&s->mutex);
    return serial;
}
ViewResult *view_take(ViewService *s)
{
    pthread_mutex_lock(&s->mutex);
    ViewResult *r = s->ready;
    s->ready = NULL;
    if (!r && s->statistics_ready) {
        r = s->statistics_ready;
        s->statistics_ready = NULL;
        /* Global totals belong to the session, not to a directory-selection serial. */
        r->serial = atomic_load(&s->serial);
    }
    pthread_mutex_unlock(&s->mutex);
    return r;
}
int view_busy(ViewService *s)
{
    pthread_mutex_lock(&s->mutex);
    int busy = atomic_load(&s->running) || s->pending != NULL;
    pthread_mutex_unlock(&s->mutex);
    return busy;
}
void view_save(ViewService *s, const char *left, const char *right, int selected, float scale,
               int filter, int page, float split_top, float split_bottom, float split_rows,
               int theme, int log_visible, int log_level, float split_log, const char *font_file,
               const char *font_family, const char *font_style, int font_index, int sort_left,
               int sort_right, int created_left, int created_right, const char *columns_left,
               const char *columns_right)
{
    char *v[] = {xstrdup(left),
                 xstrdup(right),
                 fmt("%d", selected),
                 fmt("%.3f", scale),
                 fmt("%d", filter),
                 fmt("%d", page),
                 fmt("%.4f", split_top),
                 fmt("%.4f", split_bottom),
                 fmt("%.4f", split_rows),
                 fmt("%d", theme),
                 fmt("%d", log_visible),
                 fmt("%d", log_level),
                 fmt("%.4f", split_log),
                 xstrdup(font_file),
                 xstrdup(font_family),
                 xstrdup(font_style),
                 fmt("%d", font_index),
                 fmt("%d", sort_left),
                 fmt("%d", sort_right),
                 fmt("%d", created_left),
                 fmt("%d", created_right),
                 xstrdup(columns_left),
                 xstrdup(columns_right)};
    _Static_assert(sizeof(v) / sizeof(v[0]) == VIEW_SETTING_COUNT, "UI settings count");
    pthread_mutex_lock(&s->mutex);
    for (int i = 0; i < VIEW_SETTING_COUNT; i++) {
        free(s->save[i]);
        s->save[i] = v[i];
    }
    s->save_pending = 1;
    pthread_cond_signal(&s->cond);
    pthread_mutex_unlock(&s->mutex);
}
void view_stop(ViewService *s)
{
    if (!s)
        return;
    atomic_store(&s->stop, 1);
    pthread_mutex_lock(&s->mutex);
    pthread_cond_signal(&s->cond);
    pthread_cond_signal(&s->statistics_cond);
    pthread_mutex_unlock(&s->mutex);
    pthread_join(s->thread, NULL);
    pthread_join(s->metrics_thread, NULL);
    pthread_join(s->statistics_thread, NULL);
    request_free(s->pending);
    view_result_free(s->ready);
    view_result_free(s->statistics_ready);
    for (int i = 0; i < VIEW_SETTING_COUNT; i++)
        free(s->save[i]);
    free(s->target_cache);
    view_cache_free(s->source_cache);
    sqlite3_close(s->reader.db);
    sqlite3_close(s->statistics_reader.db);
    pthread_mutex_destroy(&s->statistics_reader.progress_mutex);
    pthread_mutex_destroy(&s->reader.progress_mutex);
    pthread_mutex_destroy(&s->mutex);
    pthread_cond_destroy(&s->cond);
    pthread_cond_destroy(&s->statistics_cond);
    free(s);
}
