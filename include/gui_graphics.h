#ifndef CCURATE_GUI_GRAPHICS_H
#define CCURATE_GUI_GRAPHICS_H
#include <Raven.h>
#ifdef _WIN32
#include "platform/windows/graphics_windows.h"
#endif

static inline int gui_graphics_init(const RInitDesc *init)
{
#ifdef _WIN32
    return cc_win_graphics_init(init);
#else
    return Rinit_ex(init);
#endif
}
static inline void gui_graphics_shutdown(void)
{
#ifdef _WIN32
    cc_win_graphics_shutdown();
#else
    Rshutdown();
#endif
}
#endif
