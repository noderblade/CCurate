#ifndef CCURATE_GUI_LAYOUT_H
#define CCURATE_GUI_LAYOUT_H

/* Panel geometry, hit testing and split proportions. */
#include "gui_types.h"

typedef struct {
    float x, y, w, h;
} GuiRect;
typedef struct {
    GuiRect pane[5], split[4];
} GuiGeometry;

typedef struct {
    GuiRect body;
    float widths[5], width;
} GuiTable;

GuiTable gui_table(Gui *ui, int side);
int gui_table_visible(Gui *ui, int side);
float gui_pane_top(Gui *ui);
GuiGeometry gui_geometry(Gui *ui);
int gui_contains(GuiRect r, float x, float y);
int gui_pane_at(GuiGeometry g, float x, float y);
float gui_pane_height(Gui *ui);
void gui_reset_layout(Gui *ui);

#endif /* CCURATE_GUI_LAYOUT_H */
