#ifndef CCURATE_VIEW_CACHE_H
#define CCURATE_VIEW_CACHE_H
#include "curator.h"

/* Worker-owned, bounded source-page cache. SQLite data_version invalidates it
 * after commits on other connections, including scans and curation operations. */
typedef struct ViewCache ViewCache;
ViewCache *view_cache_create(void);
void view_cache_free(ViewCache *cache);
void view_cache_refresh(ViewCache *cache, Catalog *catalog);
int view_cache_load(ViewCache *cache, const ViewRequest *request, ViewResult *result);
void view_cache_store(ViewCache *cache, Catalog *catalog, const ViewRequest *request,
                      const ViewResult *result);
#endif
