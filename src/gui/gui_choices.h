#ifndef CCURATE_GUI_CHOICES_H
#define CCURATE_GUI_CHOICES_H

/* Log-level and color-scheme popup controls. */
#include "gui_types.h"
#include <Raven.h>

void gui_dropdown_open(Gui *ui, int type);
void gui_dropdown_paint(Gui *ui, RPainter *p);
int gui_dropdown_event(Gui *ui, const REvent *e);

#endif /* CCURATE_GUI_CHOICES_H */
