#ifndef CCURATE_GUI_DETAILS_H
#define CCURATE_GUI_DETAILS_H

#include "gui_layout.h"
#include <Raven.h>

GuiRect gui_details_body(Gui *ui);
void gui_details_paint(Gui *ui, RPainter *p);
int gui_details_event(Gui *ui, const REvent *event);
int gui_details_command(Gui *ui, const char *command);
const char *gui_details_tip(Gui *ui);
void gui_details_loaded(Gui *ui);

#endif
