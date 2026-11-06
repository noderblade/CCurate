#ifndef CCURATE_GUI_ACTIVITY_H
#define CCURATE_GUI_ACTIVITY_H

/* Activity panel rendering, scrolling and severity choices. */
#include "gui_layout.h"
#include "gui_types.h"
#include <Raven.h>

int gui_log_level_index(Gui *ui);
void gui_refresh_log(Gui *ui);
GuiRect gui_log_button(GuiGeometry g, int index);
void gui_log_paint(Gui *ui, RPainter *p, GuiGeometry g);
int gui_log_event(Gui *ui, const REvent *e, GuiGeometry g);
const char *gui_activity_level_name(int index);
int gui_activity_level_id(int index);

#endif /* CCURATE_GUI_ACTIVITY_H */
