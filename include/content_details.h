#ifndef CCURATE_CONTENT_DETAILS_H
#define CCURATE_CONTENT_DETAILS_H
#include "curator.h"

/* Read-only, paged presentation of recorded paths and per-path version changes. */
void content_occurrences(Catalog *c, Buf *out, const char *hash, const char *empty_destination,
                         int page);
void content_version_history(Catalog *c, Buf *out, int64_t entry_id, int64_t selected_version,
                             DetailHistory *history);
#endif
