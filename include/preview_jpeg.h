#ifndef CCURATE_PREVIEW_JPEG_H
#define CCURATE_PREVIEW_JPEG_H

#include "curator.h"

/* Decode directly to a bounded, oriented thumbnail. The source fd is read-only. */
int preview_jpeg(int fd, Preview *preview, Buf *details);

#endif
