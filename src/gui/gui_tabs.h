#ifndef CCURATE_GUI_TABS_H
#define CCURATE_GUI_TABS_H
#include "gui_types.h"
#include <Raven.h>
void gui_tabs_select(Gui *ui, int tab);
void gui_tabs_paint(Gui *ui, RPainter *p);
int gui_tabs_event(Gui *ui, const REvent *event);
#endif
