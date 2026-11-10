#ifndef CCURATE_TARGET_DETAILS_H
#define CCURATE_TARGET_DETAILS_H
#include "curator.h"

/* Read actual destination metadata and indexed provenance in the view worker.
 * Does not hash content, follow symlinks or change the catalog. Caller owns text. */
char *target_details_page(Catalog *c, const char *path, int page);

#endif
