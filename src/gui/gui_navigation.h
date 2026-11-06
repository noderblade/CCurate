#ifndef CCURATE_GUI_NAVIGATION_H
#define CCURATE_GUI_NAVIGATION_H

#include "gui_types.h"

/* Bounded, per-pane locations for this application run. No catalog mutation. */
void gui_navigation_save(Gui *ui, int side);
void gui_navigation_restore(Gui *ui, int side, const char *path, const char *previous);
void gui_navigation_free(GuiPane *pane);

#endif
