#include "view_cache.h"
#include <stdlib.h>
#include <string.h>

enum { CACHE_PAGES = 32, CACHE_BYTES = 16 * 1024 * 1024 };
typedef struct {
    char *path, *search;
    int page, sort, historical, unsecured, fast, count;
    int64_t total;
    uint64_t used;
    size_t bytes;
    Entry *rows;
} CachedPage;
struct ViewCache {
    CachedPage pages[CACHE_PAGES];
    int64_t version;
    uint64_t clock;
    size_t bytes;
};

static size_t clone_rows(Entry **destination, const Entry *source, int count)
{
    Entry *rows = xmalloc((size_t)count * sizeof(*rows));
    size_t bytes = (size_t)count * sizeof(*rows);
    for (int i = 0; i < count; i++) {
        rows[i] = source[i];
        char **out[] = {&rows[i].path, &rows[i].parent, &rows[i].name,  &rows[i].kind,
                        &rows[i].hash, &rows[i].dest,   &rows[i].state, &rows[i].error};
        for (unsigned j = 0; j < sizeof(out) / sizeof(out[0]); j++) {
            *out[j] = xstrdup(*out[j]);
            bytes += strlen(*out[j]) + 1;
        }
    }
    *destination = rows;
    return bytes;
}
static void clear_page(ViewCache *cache, CachedPage *page)
{
    cache->bytes -= page->bytes;
    free(page->path);
    free(page->search);
    entries_free(page->rows, page->count);
    memset(page, 0, sizeof(*page));
}
static void clear(ViewCache *cache)
{
    for (int i = 0; i < CACHE_PAGES; i++)
        clear_page(cache, &cache->pages[i]);
}
static int64_t data_version(Catalog *catalog)
{
    sqlite3_stmt *q = sql_prepare(catalog, "PRAGMA data_version");
    int64_t version = q && sqlite3_step(q) == SQLITE_ROW ? sqlite3_column_int64(q, 0) : -1;
    sqlite3_finalize(q);
    return version;
}
ViewCache *view_cache_create(void)
{
    ViewCache *cache = xmalloc(sizeof(*cache));
    cache->version = -1;
    return cache;
}
void view_cache_free(ViewCache *cache)
{
    if (cache) {
        clear(cache);
        free(cache);
    }
}
void view_cache_refresh(ViewCache *cache, Catalog *catalog)
{
    int64_t version = data_version(catalog);
    if (version < 0 || version != cache->version)
        clear(cache);
    cache->version = version;
}
static int matches(const CachedPage *page, const ViewRequest *r, int number)
{
    return page->path && !strcmp(page->path, r->path[1]) && !strcmp(page->search, r->search[1]) &&
           page->page == number && page->sort == r->sort[1] && page->historical == r->historical &&
           page->unsecured == r->unsecured && page->fast == r->fast;
}
int view_cache_load(ViewCache *cache, const ViewRequest *r, ViewResult *out)
{
    for (int i = 0; i < CACHE_PAGES; i++) {
        CachedPage *page = &cache->pages[i];
        if (!matches(page, r, r->page[1]))
            continue;
        /* A saved page is usable for an anchor only if that exact path is on it.
         * Otherwise the normal rank query resolves a changed page/sort position. */
        if (r->anchor[1] && *r->anchor[1]) {
            int found = 0;
            for (int j = 0; j < page->count; j++)
                found |= !strcmp(page->rows[j].path, r->anchor[1]);
            if (!found)
                continue;
        }
        clone_rows(&out->rows[1], page->rows, page->count);
        out->count[1] = page->count;
        out->page[1] = page->page;
        out->total[1] = page->total;
        page->used = ++cache->clock;
        return 1;
    }
    return 0;
}
void view_cache_store(ViewCache *cache, Catalog *catalog, const ViewRequest *r,
                      const ViewResult *out)
{
    int64_t version = data_version(catalog);
    if (version < 0 || version != cache->version) {
        clear(cache);
        cache->version = version;
        return; /* Never stamp an old result with a newer data version. */
    }
    CachedPage fresh = {.path = xstrdup(r->path[1]),
                        .search = xstrdup(r->search[1]),
                        .page = out->page[1],
                        .sort = r->sort[1],
                        .historical = r->historical,
                        .unsecured = r->unsecured,
                        .fast = r->fast,
                        .count = out->count[1],
                        .total = out->total[1],
                        .used = ++cache->clock};
    fresh.bytes = clone_rows(&fresh.rows, out->rows[1], out->count[1]) + strlen(fresh.path) +
                  strlen(fresh.search) + 2;
    if (fresh.bytes > CACHE_BYTES) {
        free(fresh.path);
        free(fresh.search);
        entries_free(fresh.rows, fresh.count);
        return;
    }
    int slot = -1;
    for (int i = 0; i < CACHE_PAGES; i++)
        if (matches(&cache->pages[i], r, out->page[1])) {
            slot = i;
            clear_page(cache, &cache->pages[i]);
            break;
        }
    if (slot < 0) {
        int oldest = 0;
        for (int i = 1; i < CACHE_PAGES; i++)
            if (cache->pages[i].used < cache->pages[oldest].used)
                oldest = i;
        clear_page(cache, &cache->pages[oldest]);
        slot = oldest;
    }
    while (cache->bytes + fresh.bytes > CACHE_BYTES) {
        int oldest = -1;
        for (int i = 0; i < CACHE_PAGES; i++)
            if (cache->pages[i].path &&
                (oldest < 0 || cache->pages[i].used < cache->pages[oldest].used))
                oldest = i;
        if (oldest < 0)
            abort();
        clear_page(cache, &cache->pages[oldest]);
    }
    cache->pages[slot] = fresh;
    cache->bytes += fresh.bytes;
}
