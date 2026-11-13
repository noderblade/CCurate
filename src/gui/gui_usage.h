#ifndef CCURATE_GUI_USAGE_H
#define CCURATE_GUI_USAGE_H

#include "gui_types.h"
#include <Raven.h>

void gui_usage_init(Gui *ui);
void gui_usage_shutdown(Gui *ui);
void gui_usage_tick(Gui *ui);
void gui_usage_save(Gui *ui);
void gui_usage_activate(Gui *ui);
int gui_usage_command(Gui *ui, const char *command);
void gui_usage_paint(Gui *ui, RPainter *p);
int gui_usage_event(Gui *ui, const REvent *event);
void gui_usage_scroll(Gui *ui, int id, double value);
int gui_usage_popup(Gui *ui);

#endif
