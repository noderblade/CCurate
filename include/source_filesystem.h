#ifndef CCURATE_SOURCE_FILESYSTEM_H
#define CCURATE_SOURCE_FILESYSTEM_H
#include "curator.h"

/* NULL/0 means unavailable, never a guessed identity derived from st_dev.
 * A negative result is a database error. Runtime device numbers only index
 * an in-memory cache whose open descriptors pin the mounted filesystems. */
int64_t source_filesystem(Catalog *c, int parent, const char *leaf, const struct stat *st);
void source_filesystems_close(Catalog *c);
void source_filesystem_details(Catalog *c, Buf *out, int64_t id);

/* Platform probe: return an owned descriptor pinning st->st_dev, or -1.
 * identity/name are owned strings; identity may be NULL if unavailable. */
int source_filesystem_probe(int parent, const char *leaf, const struct stat *st, char **identity,
                            char **name);
#endif
