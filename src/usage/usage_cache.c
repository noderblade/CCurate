#include "usage_internal.h"
#include <stdlib.h>
#include <string.h>

/* Navigation pages and chart geometry inputs share a bounded LRU. Selection only
 * changes details: arrowing through a folder never reruns its sort or chart SQL. */
enum { CACHE_PAGES = 16, CACHE_BYTES = 16 * 1024 * 1024 };
typedef struct {
    UsageRequest key;
    UsageResult *result;
    uint64_t used;
    size_t bytes;
} Page;
struct UsagePageCache {
    Page pages[CACHE_PAGES];
    uint64_t clock;
    size_t bytes;
};

static UsageRow copy_row(const UsageRow *from)
{
    UsageRow row = *from;
    row.path = xstrdup(from->path);
    row.name = xstrdup(from->name);
    row.kind = xstrdup(from->kind);
    return row;
}
static UsageResult *copy_result(const UsageResult *from)
{
    UsageResult *r = xmalloc(sizeof(*r));
    *r = *from;
    r->path = xstrdup(from->path);
    r->error = r->details = NULL;
    r->rows = xmalloc((size_t)from->count * sizeof(*r->rows));
    for (int i = 0; i < r->count; i++)
        r->rows[i] = copy_row(from->rows + i);
    for (int i = 0; i < r->node_count; i++)
        r->nodes[i].row = copy_row(&from->nodes[i].row);
    return r;
}
static int matches(const UsageRequest *a, const UsageRequest *b)
{
    if (strcmp(a->path, b->path) || a->page != b->page || a->sort != b->sort ||
        a->descending != b->descending || a->keep != b->keep || a->depth != b->depth ||
        a->expanded_count != b->expanded_count)
        return 0;
    for (int i = 0; i < a->expanded_count; i++)
        if (strcmp(a->expanded[i], b->expanded[i]))
            return 0;
    return 1;
}
static void drop(struct UsagePageCache *c, int index)
{
    Page *p = c->pages + index;
    c->bytes -= p->bytes;
    usage_result_free(p->result);
    usage_request_clear(&p->key);
    memset(p, 0, sizeof(*p));
}
UsageResult *usage_cache_load(UsageDb *db, const UsageRequest *request)
{
    if (!db->pages)
        return NULL;
    for (int i = 0; i < CACHE_PAGES; i++) {
        Page *p = db->pages->pages + i;
        if (p->result && p->result->revision == db->indexed_revision && matches(&p->key, request)) {
            p->used = ++db->pages->clock;
            return copy_result(p->result);
        }
    }
    return NULL;
}
static size_t row_bytes(const UsageRow *r)
{
    return sizeof(*r) + strlen(r->path) + strlen(r->name) + strlen(r->kind) + 3;
}
void usage_cache_store(UsageDb *db, const UsageRequest *request, const UsageResult *result)
{
    if (!db->pages)
        db->pages = xmalloc(sizeof(*db->pages));
    struct UsagePageCache *c = db->pages;
    size_t bytes = sizeof(*result) + strlen(result->path) + strlen(request->path) + 2;
    for (int i = 0; i < result->count; i++)
        bytes += row_bytes(result->rows + i);
    for (int i = 0; i < result->node_count; i++)
        bytes += row_bytes(&result->nodes[i].row);
    for (int i = 0; i < request->expanded_count; i++)
        bytes += strlen(request->expanded[i]) + 1;
    if (bytes > CACHE_BYTES)
        return;
    int slot = -1;
    for (int i = 0; i < CACHE_PAGES; i++) {
        if (c->pages[i].result && (c->pages[i].result->revision != db->indexed_revision ||
                                   matches(&c->pages[i].key, request)))
            drop(c, i);
        if (!c->pages[i].result)
            slot = i;
    }
    while (slot < 0 || c->bytes + bytes > CACHE_BYTES) {
        int oldest = -1;
        for (int i = 0; i < CACHE_PAGES; i++)
            if (c->pages[i].result && (oldest < 0 || c->pages[i].used < c->pages[oldest].used))
                oldest = i;
        if (oldest < 0)
            break;
        drop(c, oldest);
        slot = oldest;
    }
    if (slot < 0)
        return;
    Page *p = c->pages + slot;
    usage_request_copy(&p->key, request);
    free(p->key.selection);
    p->key.selection = xstrdup("");
    p->result = copy_result(result);
    p->used = ++c->clock;
    p->bytes = bytes;
    c->bytes += bytes;
}
void usage_cache_free(UsageDb *db)
{
    if (!db->pages)
        return;
    for (int i = 0; i < CACHE_PAGES; i++)
        drop(db->pages, i);
    free(db->pages);
    db->pages = NULL;
}
