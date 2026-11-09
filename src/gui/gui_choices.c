#include "gui_choices.h"
#include "gui_activity.h"
#include "gui_browser.h"
#include "gui_columns.h"
#include "gui_controller.h"
#include "gui_layout.h"
#include "gui_render.h"
#include "gui_scroll.h"
#include "gui_state.h"
#include "gui_theme.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Log-level, color-scheme and sort-order popup controls. */

static int gui_dropdown_count(Gui *ui);
static GuiRect gui_dropdown_rect(Gui *ui, int *first, int *visible);
static void gui_dropdown_choose(Gui *ui);

static int gui_dropdown_count(Gui *ui)
{
    return ui->dropdown == 1   ? 5
           : ui->dropdown == 3 ? SORT_COUNT + 2
           : ui->dropdown == 4 ? GUI_FILE_COLUMNS
                               : gui_theme_count();
}

static GuiRect gui_dropdown_rect(Gui *ui, int *first, int *visible)
{
    int n = gui_dropdown_count(ui);
    *visible = (int)fmaxf(1, fminf(n, (ui->height - 100) / 30));
    ui->dropdown_offset = (int)fmaxf(0, fminf(n - *visible, ui->dropdown_offset));
    *first = ui->dropdown_offset;
    float width = fminf(440, ui->width - 24), height = 48 + *visible * 30;
    GuiRect r = {(ui->width - width) / 2, (ui->height - height) / 2, width, height};
    if (ui->dropdown == 1) {
        GuiRect b = gui_log_button(gui_geometry(ui), 0);
        r.x = fmaxf(12, fminf(b.x, ui->width - width - 12));
        r.y = fmaxf(12, fminf(b.y + b.h, ui->height - height - 48));
    }
    return r;
}

void gui_dropdown_open(Gui *ui, int type)
{
    ui->menu = 0;
    ui->dropdown = type;
    ui->dropdown_row = type == 1   ? gui_log_level_index(ui)
                       : type == 3 ? ui->pane[ui->sort_side].sort & SORT_COLUMN_MASK
                       : type == 4 ? 0
                                   : ui->theme;
    ui->dropdown_offset = 0;
    int first, visible;
    gui_dropdown_rect(ui, &first, &visible);
    if (ui->dropdown_row >= visible)
        ui->dropdown_offset = ui->dropdown_row - visible + 1;
    Rwidget_set_tooltip(ui->canvas, "");
    Rwidget_focus(ui->canvas);
    Rwidget_invalidate(ui->canvas);
}

static void gui_dropdown_choose(Gui *ui)
{
    if (ui->dropdown == 1) {
        ui->log_level = gui_activity_level_id(ui->dropdown_row);
        ui->log_revision = UINT64_MAX;
        ui->log_scroll = ui->log_horizontal = 0;
        gui_refresh_log(ui);
    } else if (ui->dropdown == 4) {
        ui->column_index = ui->dropdown_row;
        ui->dropdown = 0;
        gui_column_prompt(ui);
        return;
    } else if (ui->dropdown == 3) {
        int sort = ui->pane[ui->sort_side].sort;
        int column = ui->dropdown_row < SORT_COUNT ? ui->dropdown_row : sort & SORT_COLUMN_MASK;
        int descending = ui->dropdown_row == SORT_COUNT       ? 0
                         : ui->dropdown_row == SORT_COUNT + 1 ? 1
                                                              : (sort & SORT_DESC) != 0;
        gui_set_sort(ui, ui->sort_side, column, descending);
    } else
        gui_apply_theme(ui, ui->dropdown_row);
    ui->dropdown = 0;
    gui_save_ui(ui);
    Rwidget_invalidate(ui->canvas);
}

void gui_dropdown_paint(Gui *ui, RPainter *p)
{
    if (!ui->dropdown)
        return;
    int first, visible;
    GuiRect r = gui_dropdown_rect(ui, &first, &visible);
    Rdraw_rect(p, r.x + 4, r.y + 5, r.w, r.h, 8, 0, 0, 0, .35f);
    Rdraw_rect(p, r.x, r.y, r.w, r.h, 8, GUI_COLOR(ui, raised), 1);
    Rdraw_rect_outline(p, r.x, r.y, r.w, r.h, 8, 1, GUI_COLOR(ui, accent), 1);
    gui_text_clip(ui, p, r.x + 12, r.y + 12, r.w - 24, 24,
                  ui->dropdown == 1   ? "LOG LEVEL | arrows / Enter / Esc"
                  : ui->dropdown == 3 ? "SORT | arrows / Enter / Esc"
                  : ui->dropdown == 4 ? "COLUMN WIDTH | arrows / Enter / Esc"
                                      : "COLOR SCHEME | arrows / Enter / Esc",
                  GUI_COLOR(ui, muted), ui->font_size - 1);
    for (int i = first; i < first + visible; i++) {
        char label[64];
        if (ui->dropdown == 4)
            gui_column_label(ui, ui->column_side, i, label);
        float y = r.y + 42 + (i - first) * 30;
        if (i == ui->dropdown_row)
            Rdraw_rect(p, r.x + 5, y, r.w - 10, 28, 4, GUI_COLOR(ui, selection), 1);
        gui_text_clip(ui, p, r.x + 14, y + 6, r.w - 28, 24,
                      ui->dropdown == 1   ? gui_activity_level_name(i)
                      : ui->dropdown == 4 ? label
                      : ui->dropdown == 3 ? (i < SORT_COUNT    ? gui_sort_name(i)
                                             : i == SORT_COUNT ? "Ascending"
                                                               : "Descending")
                                          : gui_theme_get(i)->name,
                      GUI_COLOR(ui, text), ui->font_size);
    }
    gui_scrollbar_paint(ui, p, 13, (GuiRect){r.x + r.w - 14, r.y + 42, 10, visible * 30},
                        gui_dropdown_count(ui), visible, first, 0);
}

int gui_dropdown_event(Gui *ui, const REvent *e)
{
    if (!ui->dropdown)
        return 0;
    int first, visible;
    GuiRect r = gui_dropdown_rect(ui, &first, &visible);
    if (e->type == R_EVENT_KEY_DOWN) {
        switch (e->key) {
        case R_KEY_ESCAPE:
            ui->dropdown = 0;
            break;
        case R_KEY_UP:
            ui->dropdown_row--;
            break;
        case R_KEY_DOWN:
            ui->dropdown_row++;
            break;
        case R_KEY_HOME:
            ui->dropdown_row = 0;
            break;
        case R_KEY_END:
            ui->dropdown_row = gui_dropdown_count(ui) - 1;
            break;
        case R_KEY_ENTER:
        case R_KEY_KP_ENTER:
            gui_dropdown_choose(ui);
            return 1;
        default:
            break;
        }
    } else if (e->type == R_EVENT_MOUSE_WHEEL) {
        ui->dropdown_row += e->wheel_y > 0 ? -1 : 1;
    } else if (e->type == R_EVENT_MOUSE_MOVE ||
               (e->type == R_EVENT_MOUSE_DOWN && e->button == R_MOUSE_LEFT)) {
        int row = first + (int)((e->y - r.y - 42) / 30);
        if (gui_contains(r, e->x, e->y) && e->y >= r.y + 42 && row < first + visible) {
            ui->dropdown_row = row;
            if (e->type == R_EVENT_MOUSE_DOWN)
                gui_dropdown_choose(ui);
        } else if (e->type == R_EVENT_MOUSE_DOWN)
            ui->dropdown = 0;
    }
    if (ui->dropdown_row < 0)
        ui->dropdown_row = 0;
    if (ui->dropdown_row >= gui_dropdown_count(ui))
        ui->dropdown_row = gui_dropdown_count(ui) - 1;
    if (e->type == R_EVENT_KEY_DOWN || e->type == R_EVENT_MOUSE_WHEEL) {
        if (ui->dropdown_row < ui->dropdown_offset)
            ui->dropdown_offset = ui->dropdown_row;
        if (ui->dropdown_row >= ui->dropdown_offset + visible)
            ui->dropdown_offset = ui->dropdown_row - visible + 1;
    }
    Rwidget_invalidate(ui->canvas);
    return 1;
}
