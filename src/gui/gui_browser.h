#ifndef CCURATE_GUI_BROWSER_H
#define CCURATE_GUI_BROWSER_H

/* Paged browsing, selection, navigation and asynchronous view results. */
#include "curator.h"
#include "gui_types.h"

int gui_row_count(const GuiPane *p);
Entry *gui_selected(GuiPane *p);
int gui_selected_index(GuiPane *p);
void gui_request_view(Gui *ui, unsigned flags);
void gui_toggle_mark(GuiPane *p, int row);
void gui_clear_marks(GuiPane *p);
void gui_pane_reload(Gui *ui, int side);
void gui_apply_view(Gui *ui);
void gui_change_page(Gui *ui, int side, int page, int end);
void gui_scroll_selection(Gui *ui, GuiPane *p);
void gui_navigate(Gui *ui, int side, const char *path);
void gui_enter_directory(Gui *ui, int side);
void gui_go_parent(Gui *ui, int side);
void gui_seek_row(Gui *ui, int side, int64_t row);
void gui_set_sort(Gui *ui, int side, int column, int descending);
int gui_column_at(Gui *ui, int side, float x);
const char *gui_sort_name(int column);

#endif /* CCURATE_GUI_BROWSER_H */
