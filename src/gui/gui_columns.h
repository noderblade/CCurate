#ifndef CCURATE_GUI_COLUMNS_H
#define CCURATE_GUI_COLUMNS_H

#include "gui_types.h"
#include <Raven.h>

enum { GUI_FILE_COLUMNS = 5 };
#define GUI_COLUMN_MIN 32.0f
#define GUI_COLUMN_MAX 32768.0f

/* Shared cell formatting keeps measured widths identical to rendered text. */
typedef struct {
    char *name;
    char status[96], size[40], date[40], mode[20];
} GuiFileCells;

void gui_file_cells(Gui *ui, int side, int row, GuiFileCells *cells);
void gui_column_label(Gui *ui, int side, int column, char label[64]);
void gui_columns_restore(Gui *ui, int side, const char *value);
void gui_column_set_width(Gui *ui, int side, int column, float width);
void gui_columns_autofit(Gui *ui, int side, int column);
void gui_columns_reset(Gui *ui, int side);
void gui_column_prompt(Gui *ui);
int gui_columns_event(Gui *ui, const REvent *event);

#endif /* CCURATE_GUI_COLUMNS_H */
