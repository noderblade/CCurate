#ifndef CCURATE_GUI_MENU_H
#define CCURATE_GUI_MENU_H

/* Action definitions, footer menus and mouse/keyboard menu navigation. */
#include "gui_types.h"
#include <Raven.h>

void gui_menu_open(Gui *ui, int menu);
void gui_menu_paint(Gui *ui, RPainter *p);
int gui_menu_event(Gui *ui, const REvent *e);

#endif /* CCURATE_GUI_MENU_H */
