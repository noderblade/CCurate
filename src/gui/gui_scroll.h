#ifndef CCURATE_GUI_SCROLL_H
#define CCURATE_GUI_SCROLL_H

#include "gui_layout.h"
#include <Raven.h>

enum { GUI_SCROLLBARS = 14 };
typedef struct {
    GuiRect track, thumb;
    double value, maximum, page;
    int active, horizontal;
} GuiScrollbar;
typedef struct {
    char *source, *buffer, **lines;
    int count;
    float width, size;
    Rid font;
} GuiTextCache;

void gui_scroll_begin(Gui *ui);
void gui_scrollbar_paint(Gui *ui, RPainter *p, int id, GuiRect track, double total, double visible,
                         double value, int horizontal);
int gui_scroll_event(Gui *ui, const REvent *event);
void gui_text_panel_paint(Gui *ui, RPainter *p, int side, GuiRect body, const char *text);
void gui_text_cache_free(GuiTextCache *cache);
void gui_text_scroll_clamp(Gui *ui, int side);

#endif
