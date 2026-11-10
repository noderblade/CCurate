#ifndef CCURATE_REMOVE_TARGET_H
#define CCURATE_REMOVE_TARGET_H

#include "curator.h"

/* One indexed regular destination file. A live source copy must pass SHA-512
 * verification. Retained paths and decisions are never removed. */
int remove_target_copy(Catalog *c, const char *path);

/* Restore staged files after interruption; never resume deletion on startup. */
int recover_target_removals(Catalog *c);

#endif
