#include "gui_columns.h"
#include "gui_browser.h"
#include "gui_controller.h"
#include "gui_layout.h"
#include "gui_state.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

void gui_file_cells(Gui *ui, int side, int row, GuiFileCells *cells)
{
    GuiPane *pane = &ui->pane[side];
    Entry *e = &pane->rows[row];
    int dir = !strcmp(e->kind, "directory");
    int secured = !strcmp(e->state, "verified") || (!*e->hash && e->accepted);
    char *safe = display_path(e->name);
    cells->name = fmt("%s%s%s",
                      pane->marked[row] ? "* "
                      : e->reviewed     ? "r "
                                        : "  ",
                      safe, dir ? "/" : "");
    free(safe);
    char *state = cells->status;
    size_t length = sizeof(cells->status);
    if (side && !strncmp(e->error, "UNSCANNED:", 10))
        snprintf(state, length, "UNSCANNED");
    else if (side && e->pending)
        snprintf(state, length, "%s", *e->error ? "ERROR" : "PENDING");
    else if (dir && side && e->children < 0)
        snprintf(state, length, "%s", atomic_load(&ui->busy) ? "scanning..." : "counting...");
    else if (dir && side)
        snprintf(state, length, "%s %lld/%lld",
                 e->discarded                                 ? "REJ"
                 : *e->error                                  ? "ERROR"
                 : (e->accepted && e->children == e->secured) ? "OK"
                                                              : "--",
                 (long long)e->secured, (long long)e->children);
    else if (e->discarded)
        snprintf(state, length, "REJECTED");
    else if (*e->error)
        snprintf(state, length, "ERROR");
    else if (!strcmp(e->state, "corrupt"))
        snprintf(state, length, "CHANGED");
    else if (!strcmp(e->state, "missing") && *e->dest)
        snprintf(state, length, "MISSING");
    else if (!strcmp(e->state, "unchecked"))
        snprintf(state, length, "UNCHECKED");
    else if (e->copies > 1)
        snprintf(state, length, "%s %s%d", secured ? "OK" : "--", e->original ? "O x" : "D x",
                 e->copies);
    else
        snprintf(state, length, "%s",
                 secured                      ? "OK"
                 : side                       ? "UNCOPIED"
                 : !strcmp(e->name, META_DIR) ? "SESSION"
                                              : "");
    if (dir)
        snprintf(cells->size, sizeof(cells->size), "<DIR>");
    else
        human_bytes(e->size, cells->size);
    date_text(pane->created ? e->btime : e->mtime, cells->date);
    snprintf(cells->mode, sizeof(cells->mode), "%04llo", (unsigned long long)e->mode & 07777);
}

void gui_column_label(Gui *ui, int side, int column, char label[64])
{
    GuiPane *pane = &ui->pane[side];
    int sort = column == 3   ? (pane->created ? SORT_BTIME : SORT_MTIME)
               : column == 4 ? SORT_MODE
                             : column;
    snprintf(label, 64, "%s%s", gui_sort_name(sort),
             (pane->sort & SORT_COLUMN_MASK) == sort ? (pane->sort & SORT_DESC ? " v" : " ^") : "");
}

void gui_columns_restore(Gui *ui, int side, const char *value)
{
    float widths[GUI_FILE_COLUMNS];
    char extra;
    if (sscanf(value, "%f %f %f %f %f %c", &widths[0], &widths[1], &widths[2], &widths[3],
               &widths[4], &extra) != GUI_FILE_COLUMNS)
        return;
    for (int i = 0; i < GUI_FILE_COLUMNS; i++)
        if (!isfinite(widths[i]) ||
            (widths[i] != 0 && (widths[i] < GUI_COLUMN_MIN || widths[i] > GUI_COLUMN_MAX)))
            return;
    memcpy(ui->pane[side].column_widths, widths, sizeof(widths));
}

void gui_column_set_width(Gui *ui, int side, int column, float width)
{
    if (side < 0 || side > 1 || column < 0 || column >= GUI_FILE_COLUMNS || !isfinite(width))
        return;
    ui->pane[side].column_widths[column] = fmaxf(GUI_COLUMN_MIN, fminf(GUI_COLUMN_MAX, width));
    Rwidget_invalidate(ui->canvas);
}

static float text_width(Gui *ui, const char *text, float size, float padding)
{
    float width = 0;
    Rfont_measure_text(ui->font, size, text, &width, NULL);
    return ceilf(width) + padding;
}

void gui_columns_autofit(Gui *ui, int side, int column)
{
    GuiPane *pane = &ui->pane[side];
    float widths[GUI_FILE_COLUMNS];
    for (int i = 0; i < GUI_FILE_COLUMNS; i++) {
        char label[64];
        gui_column_label(ui, side, i, label);
        widths[i] = text_width(ui, label, ui->font_size - 1, 14);
    }
    /* Only the viewport is measured. No directory walk, SQL, off-screen page or
     * font glyph-count estimate: escaped/Unicode names use the active renderer. */
    int visible = gui_table_visible(ui, side), count = gui_row_count(pane);
    int last = count > visible ? count - visible : 0;
    int first = pane->offset < last ? pane->offset : last;
    if (first < 0)
        first = 0;
    for (int row = first; row < count && row < first + visible; row++) {
        int i = row - (*pane->path ? 1 : 0);
        if (i < 0) {
            widths[0] = fmaxf(widths[0], text_width(ui, "[..]", ui->font_size, 14));
            continue;
        }
        GuiFileCells cells;
        gui_file_cells(ui, side, i, &cells);
        const char *values[] = {cells.name, cells.status, cells.size, cells.date, cells.mode};
        for (int j = 0; j < GUI_FILE_COLUMNS; j++)
            widths[j] = fmaxf(widths[j], text_width(ui, values[j],
                                                    ui->font_size - (j == 0  ? 0
                                                                     : j < 3 ? 1
                                                                             : 2),
                                                    j == 4 ? 16 : 12));
        free(cells.name);
    }
    for (int i = 0; i < GUI_FILE_COLUMNS; i++)
        if (column < 0 || column == i)
            gui_column_set_width(ui, side, i, widths[i]);
    activity_log(ui->c, LOG_DEBUG, "view.columns", pane->path, NULL,
                 "Pane %d: fit %s to visible rows", side, column < 0 ? "all columns" : "column");
    gui_save_ui(ui);
}

void gui_columns_reset(Gui *ui, int side)
{
    memset(ui->pane[side].column_widths, 0, sizeof(ui->pane[side].column_widths));
    ui->pane[side].horizontal = 0;
    gui_save_ui(ui);
    Rwidget_invalidate(ui->canvas);
}

void gui_column_prompt(Gui *ui)
{
    char label[64], title[128], value[32];
    gui_column_label(ui, ui->column_side, ui->column_index, label);
    snprintf(title, sizeof(title), "%s width: 32-32768 pixels, or auto", label);
    snprintf(value, sizeof(value), "%.0f", gui_table(ui, ui->column_side).widths[ui->column_index]);
    gui_prompt(ui, 8, title, value);
}

static int divider_at(Gui *ui, float x, float y, int *side)
{
    GuiGeometry geometry = gui_geometry(ui);
    for (int s = 0; s < 2; s++) {
        GuiTable table = gui_table(ui, s);
        if (y < geometry.pane[s].y + 39 || y >= geometry.pane[s].y + 67 || x < table.body.x ||
            x >= table.body.x + table.body.w)
            continue;
        float edge = table.body.x - ui->pane[s].horizontal;
        for (int i = 0; i < GUI_FILE_COLUMNS; i++) {
            edge += table.widths[i];
            if (fabsf(x - edge) <= 5) {
                *side = s;
                return i;
            }
        }
    }
    return -1;
}

int gui_columns_event(Gui *ui, const REvent *event)
{
    if (ui->column_drag) {
        if (event->type == R_EVENT_MOUSE_MOVE) {
            if (fabsf(event->x - ui->column_start_x) >= 5)
                ui->click_count = 0;
            gui_column_set_width(ui, ui->column_side, ui->column_index,
                                 ui->column_start_width + event->x - ui->column_start_x);
        } else if (event->type == R_EVENT_MOUSE_UP && event->button == R_MOUSE_LEFT) {
            ui->column_drag = 0;
            gui_save_ui(ui);
        } else if (event->type == R_EVENT_KEY_DOWN && event->key == R_KEY_ESCAPE) {
            ui->pane[ui->column_side].column_widths[ui->column_index] = ui->column_previous_width;
            ui->column_drag = 0;
        }
        return 1;
    }
    if (ui->dialog || ui->menu || ui->dropdown || ui->font_dialog || ui->drag ||
        ui->scroll_drag >= 0)
        return 0;
    if (event->type != R_EVENT_MOUSE_MOVE &&
        !(event->type == R_EVENT_MOUSE_DOWN && event->button == R_MOUSE_LEFT))
        return 0;
    int side = 0, column = divider_at(ui, event->x, event->y, &side);
    ui->column_hover = column < 0 ? 0 : side * GUI_FILE_COLUMNS + column + 1;
    if (column < 0)
        return 0;
    ui->splitter_hover = 0;
    Rwidget_set_cursor(ui->canvas, R_CURSOR_RESIZE_H);
    if (event->type == R_EVENT_MOUSE_DOWN) {
        ui->focus = ui->column_side = side;
        ui->column_index = column;
        if (event->clicks == 2) {
            gui_columns_autofit(ui, side, column);
        } else {
            ui->column_drag = 1;
            ui->column_start_x = event->x;
            ui->column_start_width = gui_table(ui, side).widths[column];
            ui->column_previous_width = ui->pane[side].column_widths[column];
        }
        Rwidget_focus(ui->canvas);
    }
    return 1;
}
