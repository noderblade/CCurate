#include "gui_input.h"
#include "gui_activity.h"
#include "gui_browser.h"
#include "gui_choices.h"
#include "gui_controller.h"
#include "gui_fonts.h"
#include "gui_jobs.h"
#include "gui_layout.h"
#include "gui_menu.h"
#include "gui_open.h"
#include "gui_scroll.h"
#include "gui_state.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Keyboard and pointer dispatch for the main workspace. */

int gui_handle_event(Gui *ui, const REvent *e)
{
    /* Raven's window hook runs before its own double-click counter. Events handled
     * here never reach that counter, so track canvas clicks explicitly. */
    REvent adjusted;
    if (!ui->dialog && e->type == R_EVENT_MOUSE_DOWN && e->button == R_MOUSE_LEFT) {
        double now = Rget_time();
        ui->click_count = now - ui->last_click < .4 && fabsf(e->x - ui->click_x) < 5 &&
                                  fabsf(e->y - ui->click_y) < 5
                              ? ui->click_count + 1
                              : 1;
        ui->last_click = now;
        ui->click_x = e->x;
        ui->click_y = e->y;
        adjusted = *e;
        adjusted.clicks = ui->click_count;
        e = &adjusted;
    }
    if (ui->dialog) {
        /* X11 delivers TEXT after the consumed '/' key which opened this input. */
        if (ui->filter_opener && e->type == R_EVENT_TEXT) {
            ui->filter_opener = 0;
            if (e->codepoint == '/')
                return 1;
        }
        if (e->type == R_EVENT_KEY_DOWN || e->type == R_EVENT_KEY_UP)
            ui->filter_opener = 0;
        if (e->type == R_EVENT_KEY_DOWN && e->key == R_KEY_ESCAPE) {
            gui_close_prompt(ui);
            return 1;
        }
        if (e->type == R_EVENT_KEY_DOWN && (e->key == R_KEY_ENTER || e->key == R_KEY_KP_ENTER)) {
            gui_prompt_accept(ui);
            return 1;
        }
        return 0;
    }
    if (gui_scroll_event(ui, e))
        return 1;
    if (gui_font_picker_event(ui, e))
        return 1;
    if (gui_dropdown_event(ui, e))
        return 1;
    if (gui_menu_event(ui, e))
        return 1;
    GuiGeometry g = gui_geometry(ui);
    if (gui_log_event(ui, e, g))
        return 1;
    if (e->type == R_EVENT_MOUSE_DOWN && e->button == R_MOUSE_LEFT && e->y < 37 &&
        e->x > ui->width - 170) {
        char v[32];
        snprintf(v, sizeof(v), "%.1f", ui->scale * 100);
        gui_prompt(ui, 6, "Whole UI scale: 50 - 400 percent", v);
        return 1;
    }
    if (e->type == R_EVENT_MOUSE_DOWN && e->button == R_MOUSE_LEFT) {
        for (int i = 0; i < 4; i++)
            if (gui_contains(g.split[i], e->x, e->y)) {
                if (e->clicks >= 2) {
                    if (i == 0)
                        ui->split_top = .5f;
                    else if (i == 1)
                        ui->split_bottom = .5f;
                    else if (i == 2)
                        ui->split_rows = .53f;
                    else
                        ui->split_log = .25f;
                    gui_save_ui(ui);
                } else
                    ui->drag = i + 1;
                return 1;
            }
        int side = gui_pane_at(g, e->x, e->y);
        if (side >= 0)
            ui->focus = side;
        if (side >= 0 && side < 2) {
            GuiPane *p = ui->pane + side;
            GuiRect r = g.pane[side];
            if (e->y >= r.y + 39 && e->y < r.y + 67) {
                int column = gui_column_at(ui, side, e->x);
                if (column >= 0)
                    gui_set_sort(ui, side, column,
                                 (p->sort & SORT_COLUMN_MASK) == column ? !(p->sort & SORT_DESC)
                                                                        : 0);
                Rwidget_focus(ui->canvas);
                return 1;
            }
            int row = p->offset + (int)((e->y - r.y - 70) / ui->row_height);
            GuiTable table = gui_table(ui, side);
            if (gui_contains(table.body, e->x, e->y) && row >= 0 &&
                row < p->offset + gui_table_visible(ui, side) && row < gui_row_count(p)) {
                p->selected = row;
                if (e->modifiers & R_MOD_CTRL)
                    gui_toggle_mark(p, gui_selected_index(p));
                if (side)
                    gui_request_preview(ui, 1);
                if (e->clicks == 2)
                    gui_open_selected(ui);
            }
        }
        Rwidget_focus(ui->canvas);
        return 1;
    }
    if (e->type == R_EVENT_MOUSE_UP && e->button == R_MOUSE_LEFT && ui->drag) {
        ui->drag = 0;
        gui_save_ui(ui);
        return 1;
    }
    if (e->type == R_EVENT_MOUSE_WHEEL) {
        int side = gui_pane_at(g, e->x, e->y);
        if (side < 0)
            return 0;
        ui->focus = side;
        int delta = (int)roundf(e->wheel_y * 3);
        if (!delta)
            delta = e->wheel_y > 0 ? 1 : -1;
        if (side >= 2) {
            int horizontal =
                (e->modifiers & R_MOD_SHIFT) != 0 || fabsf(e->wheel_x) > fabsf(e->wheel_y);
            int *v = side == 2 ? (horizontal ? &ui->preview_horizontal : &ui->preview_scroll)
                               : (horizontal ? &ui->details_horizontal : &ui->details_scroll);
            if (fabsf(e->wheel_x) > fabsf(e->wheel_y))
                delta = (int)roundf(e->wheel_x * 3);
            *v -= delta * (horizontal ? 24 : 1);
            gui_text_scroll_clamp(ui, side);
        } else {
            GuiPane *p = ui->pane + side;
            if ((e->modifiers & R_MOD_SHIFT) || fabsf(e->wheel_x) > fabsf(e->wheel_y)) {
                if (fabsf(e->wheel_x) > fabsf(e->wheel_y))
                    delta = (int)roundf(e->wheel_x * 3);
                p->horizontal -= delta * 24;
            } else {
                int64_t row = (int64_t)p->page * VIEW_PAGE_SIZE +
                              (p->scroll_pending ? p->scroll_row : p->offset - (*p->path ? 1 : 0));
                if (delta < 0 && p->offset + gui_table_visible(ui, side) >= gui_row_count(p) &&
                    (int64_t)(p->page + 1) * VIEW_PAGE_SIZE < p->total)
                    row = (int64_t)(p->page + 1) * VIEW_PAGE_SIZE;
                else
                    row -= delta;
                gui_seek_row(ui, side, row);
            }
        }
        return 1;
    }
    if (e->type == R_EVENT_MOUSE_MOVE) {
        if (ui->drag) {
            if (ui->drag < 3) {
                float ratio = fmaxf(.15f, fminf(.85f, (e->x - 18) / fmaxf(1, ui->width - 36)));
                if (ui->drag == 1)
                    ui->split_top = ratio;
                else
                    ui->split_bottom = ratio;
            } else if (ui->drag == 3)
                ui->split_rows = fmaxf(.15f, fminf(.85f, (e->y - gui_pane_top(ui) - 6) /
                                                             fmaxf(1, g.pane[0].h + g.pane[2].h)));
            else
                ui->split_log =
                    fmaxf(.15f, fminf(.85f, (ui->height - e->y - 48) /
                                                fmaxf(1, ui->height - gui_pane_top(ui) - 54)));
            return 1;
        }
        ui->splitter_hover = 0;
        for (int i = 0; i < 4; i++)
            if (gui_contains(g.split[i], e->x, e->y))
                ui->splitter_hover = i + 1;
        Rwidget_set_cursor(ui->canvas,
                           ui->splitter_hover
                               ? (ui->splitter_hover >= 3 ? R_CURSOR_RESIZE_V : R_CURSOR_RESIZE_H)
                               : R_CURSOR_ARROW);
        return 0;
    }
    if (e->type != R_EVENT_KEY_DOWN)
        return 0;
    int ctrl = !!(e->modifiers & R_MOD_CTRL), shift = !!(e->modifiers & R_MOD_SHIFT);
    GuiPane *p = ui->pane + (ui->focus == 0 ? 0 : 1);
    RKey k = e->key;
    if (ctrl && (k == R_KEY_ENTER || k == R_KEY_KP_ENTER)) {
        gui_open_selected(ui);
        return 1;
    }
    if (k == R_KEY_F10) {
        Rquit(0);
        return 1;
    }
    if (k == R_KEY_ESCAPE) {
        if (atomic_load(&ui->busy)) {
            atomic_store(&ui->c->cancel, 1);
            gui_notice(ui, "Cancelling; completed and verified copies remain saved.");
        } else {
            ui->focus = 1;
            ui->help = 0;
            gui_request_preview(ui, 1);
        }
        return 1;
    }
    if (k == R_KEY_TAB) {
        ui->focus = ui->focus == 0 ? 1 : 0;
        return 1;
    }
    if (ctrl && (e->modifiers & R_MOD_ALT) && (k == R_KEY_L || k == R_KEY_0)) {
        if (k == R_KEY_L)
            gui_layout_prompt(ui);
        else
            gui_reset_layout(ui);
        return 1;
    }
    if (ctrl && k == R_KEY_L) {
        gui_command(ui, shift ? "logfilter" : "log");
        return 1;
    }
    if (ctrl && shift && k == R_KEY_D) {
        ui->log_visible = 1;
        ui->focus = 4;
        gui_refresh_log(ui);
        gui_save_ui(ui);
        return 1;
    }
    if (ctrl && (e->modifiers & R_MOD_ALT) && k == R_KEY_F) {
        gui_font_picker_open(ui);
        return 1;
    }
    if (ctrl && (e->modifiers & R_MOD_ALT) && k == R_KEY_T) {
        gui_command(ui, "themes");
        return 1;
    }
    if (ctrl && shift && k == R_KEY_END) {
        gui_command(ui, "logfollow");
        return 1;
    }
    if (k == R_KEY_F2) {
        gui_menu_open(ui, 1);
        return 1;
    }
    if (ctrl && (k == R_KEY_EQUAL || k == R_KEY_KP_ADD || k == R_KEY_MINUS ||
                 k == R_KEY_KP_SUBTRACT || k == R_KEY_0)) {
        ui->scale =
            k == R_KEY_0
                ? 1
                : ui->scale + ((k == R_KEY_MINUS || k == R_KEY_KP_SUBTRACT) ? -.125f : .125f);
        ui->scale = fmaxf(.5f, fminf(4, ui->scale));
        Rset_ui_scale(ui->scale);
        gui_save_ui(ui);
        return 1;
    }
    if (ctrl && k == R_KEY_COMMA) {
        char v[32];
        snprintf(v, sizeof(v), "%.1f", ui->scale * 100);
        gui_prompt(ui, 6, "Whole UI scale: 50 - 400 percent", v);
        return 1;
    }
    if (k == R_KEY_F1) {
        gui_command(ui, "help");
        return 1;
    }
    if (k == R_KEY_F9) {
        gui_prompt(ui, 5, "Command (F1: list)", "");
        return 1;
    }
    if (k == R_KEY_F5) {
        gui_copy_selected(ui, ctrl);
        return 1;
    }
    if (k == R_KEY_F6 && !ctrl) {
        Entry *t = gui_selected(ui->pane);
        if (t && !descendant(t->path, META_DIR)) {
            free(ui->prompt_source);
            ui->prompt_source = xstrdup(t->path);
            gui_prompt(ui, 2, "Move on destination: new path from its root", t->path);
        }
        return 1;
    }
    if (k == R_KEY_F7) {
        gui_prompt(ui, 1, "New directory in the left pane", "");
        return 1;
    }
    if (k == R_KEY_F8) {
        gui_mark_selected(ui, shift ? "restore" : "reject");
        return 1;
    }
    if (ctrl && k == R_KEY_P) {
        Entry *s = gui_selected(ui->pane + 1);
        gui_mark_selected(ui, s && s->reviewed ? "unreview" : "review");
        return 1;
    }
    if (ctrl && k == R_KEY_R) {
        gui_start_job(ui, "scan", NULL, NULL, shift);
        return 1;
    }
    if (ctrl && k == R_KEY_H) {
        gui_command(ui, "history");
        return 1;
    }
    if (ctrl && k == R_KEY_F) {
        gui_command(ui, "filter");
        return 1;
    }
    if (ctrl && k == R_KEY_A) {
        gui_command(ui, "audit");
        return 1;
    }
    if (ctrl && k == R_KEY_B) {
        gui_command(ui, "balance");
        return 1;
    }
    if (ctrl && k == R_KEY_E) {
        gui_command(ui, "reconcile");
        return 1;
    }
    if (ctrl && k == R_KEY_X) {
        gui_command(ui, "hex");
        return 1;
    }
    if (ctrl && k == R_KEY_S) {
        gui_command(ui, shift ? "reverse" : "sort");
        return 1;
    }
    if (ctrl && k == R_KEY_T) {
        p->created = !p->created;
        int column = p->sort & SORT_COLUMN_MASK;
        if (column == SORT_MTIME || column == SORT_BTIME)
            gui_set_sort(ui, ui->focus == 0 ? 0 : 1, p->created ? SORT_BTIME : SORT_MTIME,
                         p->sort & SORT_DESC);
        else
            gui_save_ui(ui);
        return 1;
    }
    if (ctrl && k == R_KEY_V && ui->history) {
        gui_request_view(ui, VIEW_CYCLE);
        return 1;
    }
    if (ctrl && k == R_KEY_D) {
        ui->focus = 3;
        return 1;
    }
    if (k == R_KEY_F3 && !ctrl) {
        ui->focus = ui->focus == 2 ? 1 : 2;
        return 1;
    }
    if (ctrl && (k == R_KEY_F3 || k == R_KEY_F6)) {
        gui_set_sort(ui, ui->focus == 0 ? 0 : 1, k == R_KEY_F6 ? SORT_SIZE : SORT_NAME,
                     k == R_KEY_F6);
        return 1;
    }
    if (ctrl && (e->modifiers & R_MOD_ALT) && (k == R_KEY_PAGE_DOWN || k == R_KEY_PAGE_UP)) {
        ui->occurrence_page += k == R_KEY_PAGE_DOWN ? 1 : -1;
        if (ui->occurrence_page < 0)
            ui->occurrence_page = 0;
        ui->help = 0;
        ui->details_scroll = 0;
        ui->focus = 3;
        gui_request_view(ui, VIEW_DETAILS);
        return 1;
    }
    if (ctrl && (k == R_KEY_PAGE_DOWN || k == R_KEY_PAGE_UP)) {
        int side = ui->focus == 0 ? 0 : 1;
        int next = p->page + (k == R_KEY_PAGE_DOWN ? 1 : -1);
        if (next >= 0 && (int64_t)next * VIEW_PAGE_SIZE < p->total)
            gui_change_page(ui, side, next, k == R_KEY_PAGE_UP);
        return 1;
    }
    if (k == R_KEY_SLASH) {
        gui_prompt(ui, 3, "Name filter (leave empty to clear)", p->search);
        ui->filter_opener = 1;
        return 1;
    }
    if (ctrl && k == R_KEY_G) {
        gui_prompt(ui, 4, "Directory relative to root (empty = root)", p->path);
        return 1;
    }
    if (k == R_KEY_LEFT_BRACKET || k == R_KEY_RIGHT_BRACKET) {
        ui->page += k == R_KEY_RIGHT_BRACKET ? 1 : -1;
        if (ui->page < 0)
            ui->page = 0;
        if (ui->preview.pages && ui->page >= ui->preview.pages)
            ui->page = ui->preview.pages - 1;
        ui->preview_version = -1;
        gui_request_preview(ui, 0);
        return 1;
    }
    if (ui->focus >= 2) {
        int *v = ui->focus == 2 ? &ui->preview_scroll : &ui->details_scroll;
        int *horizontal = ui->focus == 2 ? &ui->preview_horizontal : &ui->details_horizontal;
        if (k == R_KEY_UP)
            (*v)--;
        else if (k == R_KEY_DOWN)
            (*v)++;
        else if (k == R_KEY_PAGE_UP)
            *v -= 15;
        else if (k == R_KEY_PAGE_DOWN)
            *v += 15;
        else if (k == R_KEY_HOME)
            *v = 0;
        else if (k == R_KEY_END)
            *v = (int)ui->scrollbars[ui->focus * 2].maximum;
        else if (k == R_KEY_LEFT)
            *horizontal -= 64;
        else if (k == R_KEY_RIGHT)
            *horizontal += 64;
        else
            return 0;
        gui_text_scroll_clamp(ui, ui->focus);
        return 1;
    }
    if ((e->modifiers & R_MOD_ALT) && (k == R_KEY_LEFT || k == R_KEY_RIGHT)) {
        p->horizontal += k == R_KEY_RIGHT ? 64 : -64;
        return 1;
    }
    if (k == R_KEY_ENTER || k == R_KEY_RIGHT) {
        gui_enter_directory(ui, ui->focus);
        return 1;
    }
    if (k == R_KEY_BACKSPACE || k == R_KEY_LEFT) {
        gui_go_parent(ui, ui->focus);
        return 1;
    }
    if (k == R_KEY_INSERT || k == R_KEY_SPACE) {
        int i = gui_selected_index(p);
        if (i >= 0 && i < p->n)
            gui_toggle_mark(p, i);
        if (k == R_KEY_INSERT && p->selected + 1 < gui_row_count(p))
            p->selected++;
        if (ui->focus == 1)
            gui_request_preview(ui, 1);
        return 1;
    }
    if (k == R_KEY_KP_MULTIPLY || (k == R_KEY_8 && shift)) {
        for (int i = 0; i < p->n; i++)
            gui_toggle_mark(p, i);
        return 1;
    }
    int side = ui->focus == 0 ? 0 : 1;
    if ((k == R_KEY_DOWN || k == R_KEY_PAGE_DOWN) && p->selected >= gui_row_count(p) - 1 &&
        (int64_t)(p->page + 1) * VIEW_PAGE_SIZE < p->total) {
        gui_change_page(ui, side, p->page + 1, 0);
        return 1;
    }
    if ((k == R_KEY_UP || k == R_KEY_PAGE_UP) && p->selected <= (*p->path ? 1 : 0) && p->page > 0) {
        gui_change_page(ui, side, p->page - 1, 1);
        return 1;
    }
    if (k == R_KEY_HOME && p->page > 0) {
        gui_change_page(ui, side, 0, 0);
        return 1;
    }
    if (k == R_KEY_END && (int64_t)(p->page + 1) * VIEW_PAGE_SIZE < p->total) {
        gui_change_page(ui, side, -1, 1);
        return 1;
    }
    int page = gui_table_visible(ui, side);
    if (page < 1)
        page = 1;
    if (k == R_KEY_UP)
        p->selected--;
    else if (k == R_KEY_DOWN)
        p->selected++;
    else if (k == R_KEY_PAGE_UP)
        p->selected -= page;
    else if (k == R_KEY_PAGE_DOWN)
        p->selected += page;
    else if (k == R_KEY_HOME)
        p->selected = 0;
    else if (k == R_KEY_END)
        p->selected = gui_row_count(p) - 1;
    else
        return 0;
    if (p->selected < 0)
        p->selected = 0;
    if (p->selected >= gui_row_count(p))
        p->selected = gui_row_count(p) > 0 ? gui_row_count(p) - 1 : 0;
    gui_scroll_selection(ui, p);
    if (ui->focus == 1)
        gui_request_preview(ui, 1);
    return 1;
}
