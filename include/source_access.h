#ifndef CCURATE_SOURCE_ACCESS_H
#define CCURATE_SOURCE_ACCESS_H
#include "curator.h"

/* Best-effort writer discovery; a changing file must still fail verification. */
int source_writer_busy(Catalog *c, const struct stat *st, int fd);
void source_writers_reset(Catalog *c);
#endif
