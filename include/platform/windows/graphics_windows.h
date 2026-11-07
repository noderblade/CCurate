#ifndef CCURATE_GRAPHICS_WINDOWS_H
#define CCURATE_GRAPHICS_WINDOWS_H
#include <Raven.h>

/* Prefer system Vulkan, then use the packaged CPU driver through Raven's public
 * external-device API. This module never installs or registers a system driver. */
int cc_win_graphics_init(const RInitDesc *init);
void cc_win_graphics_shutdown(void);

#endif
