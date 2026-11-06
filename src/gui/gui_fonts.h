#ifndef CCURATE_GUI_FONTS_H
#define CCURATE_GUI_FONTS_H

/* Installed monospace font chooser and font resource lifetime. */
#include "gui_types.h"
#include <Raven.h>

void gui_font_picker_open(Gui *ui);
void gui_font_tick(Gui *ui);
void gui_font_picker_paint(Gui *ui, RPainter *p);
int gui_font_picker_event(Gui *ui, const REvent *e);
void gui_font_shutdown(Gui *ui);

#endif /* CCURATE_GUI_FONTS_H */
