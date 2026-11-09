#ifndef CCURATE_SOURCE_REFRESH_H
#define CCURATE_SOURCE_REFRESH_H
#include "curator.h"

/* All calls use the catalog's single operation worker. No source writes. */
int source_needs_initial_scan(Catalog *c);
int refresh_directory(Catalog *c, const char *path);
int refresh_copy_source(Catalog *c, const char *path);

/* Catalog-side metadata observation; refresh.c owns the temporary work tables. */
int catalog_observe(Catalog *c, const char *path, int64_t generation);
#endif
