#include "gui_activity.h"
#include "gui_controller.h"
#include "gui_state.h"
#include "gui_usage_state.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static int is_expanded(GuiUsage *u, const char *path)
{
    for (int i = 0; i < u->request.expanded_count; i++)
        if (!strcmp(u->request.expanded[i], path))
            return 1;
    return 0;
}
static void popup_apply(Gui *ui)
{
    GuiUsage *u = ui->usage;
    if (u->popup == 1) {
        if (u->popup_row)
            u->columns ^= 1u << u->popup_row;
        gui_usage_save(ui);
    } else {
        u->request.sort = u->popup_row;
        u->request.descending = u->popup_row != USAGE_NAME;
        u->popup = 0;
        u->request.page = 0;
        gui_usage_request(ui);
    }
}
static int popup_event(Gui *ui, const REvent *e, GuiUsageGeometry g)
{
    GuiUsage *u = ui->usage;
    if (!u->popup)
        return 0;
    GuiRect r = {g.tree.x + 8, g.tree.y + 30, fminf(330, ui->width - 40), USAGE_COLUMNS * 30 + 62};
    r.y = fmaxf(10, fminf(r.y, ui->height - r.h - 45));
    if (e->type == R_EVENT_KEY_DOWN) {
        if (e->key == R_KEY_ESCAPE)
            u->popup = 0;
        else if (e->key == R_KEY_UP)
            u->popup_row = (u->popup_row + USAGE_COLUMNS - 1) % USAGE_COLUMNS;
        else if (e->key == R_KEY_DOWN || e->key == R_KEY_TAB)
            u->popup_row = (u->popup_row + 1) % USAGE_COLUMNS;
        else if (e->key == R_KEY_ENTER || e->key == R_KEY_SPACE)
            popup_apply(ui);
    } else if (e->type == R_EVENT_MOUSE_DOWN && e->button == R_MOUSE_LEFT) {
        int row = (int)((e->y - r.y - 34) / 30);
        if (gui_contains(r, e->x, e->y) && e->y >= r.y + 34 && row >= 0 && row < USAGE_COLUMNS) {
            u->popup_row = row;
            popup_apply(ui);
        } else if (!gui_contains(r, e->x, e->y))
            u->popup = 0;
    }
    return 1;
}
static void open_row(Gui *ui)
{
    GuiUsage *u = ui->usage;
    if (!u->result || u->selected < 0 || u->selected >= u->result->count)
        return;
    UsageRow *row = u->result->rows + u->selected;
    if (!strcmp(row->kind, "directory"))
        gui_usage_navigate(ui, row->path);
    else
        gui_usage_command(ui, "usage.reveal");
}
int gui_usage_event(Gui *ui, const REvent *e)
{
    if (!ui->tab || !ui->usage)
        return 0;
    GuiUsage *u = ui->usage;
    GuiUsageGeometry g = gui_usage_geometry(ui);
    if (popup_event(ui, e, g))
        return 1;
    if (gui_log_event(ui, e, gui_geometry(ui)))
        return 1;
    int current = u->result && !strcmp(u->result->path, u->request.path);
    if (e->type == R_EVENT_MOUSE_UP && e->button == R_MOUSE_LEFT && (u->drag || u->column_drag)) {
        u->drag = u->column_drag = 0;
        gui_usage_save(ui);
        return 1;
    }
    if (e->type == R_EVENT_MOUSE_MOVE) {
        if (u->drag) {
            if (u->drag == 1)
                u->split = ui->width >= 760
                               ? (e->x - 18) / fmaxf(1, ui->width - 36)
                               : (e->y - g.tree.y) / fmaxf(1, g.tree.h + g.chart.h + 12);
            else
                u->details_split = (g.details.y + g.details.h - e->y) /
                                   fmaxf(1, g.details.y + g.details.h - g.tree.y);
            u->split = fmaxf(.2f, fminf(.8f, u->split));
            u->details_split = fmaxf(.15f, fminf(.6f, u->details_split));
            return 1;
        }
        if (u->column_drag) {
            int col = u->column_drag - 1;
            u->widths[col] = fmaxf(55, fminf(2000, u->drag_width + e->x - u->drag_x));
            return 1;
        }
        u->column_hover = 0;
        for (int i = 0; i < USAGE_COLUMNS; i++)
            if ((u->columns & (1u << i)) && e->y >= g.columns[i].y && e->y < g.columns[i].y + 25 &&
                e->x >= g.body.x && e->x < g.body.x + g.body.w &&
                fabsf(e->x - g.columns[i].x - g.columns[i].w) < 5)
                u->column_hover = i + 1;
        u->hover = current ? gui_usage_chart_hit(ui, e->x, e->y) : -1;
        int separator =
            gui_contains(g.tree_split, e->x, e->y) || gui_contains(g.detail_split, e->x, e->y);
        Rwidget_set_cursor(ui->canvas,
                           u->column_hover ? R_CURSOR_RESIZE_H
                           : separator ? (gui_contains(g.tree_split, e->x, e->y) && ui->width >= 760
                                              ? R_CURSOR_RESIZE_H
                                              : R_CURSOR_RESIZE_V)
                                       : R_CURSOR_ARROW);
        return e->y >= gui_pane_top(ui);
    }
    if (e->type == R_EVENT_MOUSE_DOWN && e->button == R_MOUSE_LEFT) {
        if (e->y < gui_pane_top(ui))
            return 0;
        Rwidget_focus(ui->canvas);
        for (int i = 0; i < g.crumb_count; i++)
            if (gui_contains(g.crumbs[i], e->x, e->y)) {
                char *path = strndup(u->request.path, g.crumb_end[i]);
                gui_usage_navigate(ui, path);
                free(path);
                return 1;
            }
        for (int i = 0; i < USAGE_BUTTONS; i++) {
            if (i >= 13 && g.details.w <= 650)
                continue;
            if (gui_contains(g.buttons[i], e->x, e->y)) {
                gui_usage_command(ui, gui_usage_button_command(i));
                return 1;
            }
        }
        if (gui_contains(g.tree_split, e->x, e->y) || gui_contains(g.detail_split, e->x, e->y)) {
            int first = gui_contains(g.tree_split, e->x, e->y);
            if (e->clicks == 2) {
                if (first)
                    u->split = .55f;
                else
                    u->details_split = .25f;
                gui_usage_save(ui);
            } else
                u->drag = first ? 1 : 2;
            return 1;
        }
        for (int col = 0; col < USAGE_COLUMNS; col++)
            if ((u->columns & (1u << col)) && e->y >= g.columns[col].y &&
                e->y < g.columns[col].y + 25 && e->x >= g.body.x && e->x < g.body.x + g.body.w) {
                GuiRect r = g.columns[col];
                if (fabsf(e->x - r.x - r.w) < 5) {
                    if (e->clicks == 2)
                        gui_usage_fit(ui, col);
                    else {
                        u->column_drag = col + 1;
                        u->drag_x = e->x;
                        u->drag_width = u->widths[col];
                    }
                    return 1;
                }
                if (gui_contains(r, e->x, e->y)) {
                    u->request.descending =
                        u->request.sort == col ? !u->request.descending : col != USAGE_NAME;
                    u->request.sort = col;
                    u->request.page = 0;
                    u->offset = 0;
                    gui_usage_request(ui);
                    return 1;
                }
            }
        if (gui_contains(g.body, e->x, e->y) && current) {
            int row = u->offset + (int)((e->y - g.body.y) / ui->row_height);
            u->focus = 0;
            ui->focus = 1;
            if (row >= 0 && row < u->result->count) {
                float expander = g.columns[0].x + u->result->rows[row].depth * 18;
                if (e->x >= expander && e->x < expander + 24 &&
                    !strcmp(u->result->rows[row].kind, "directory"))
                    gui_usage_expand(ui, row);
                else {
                    gui_usage_select(ui, row);
                    if (e->clicks == 2)
                        open_row(ui);
                }
            }
            return 1;
        }
        if (gui_contains(g.details, e->x, e->y)) {
            u->focus = 1;
            ui->focus = 3;
            return 1;
        }
        if (current && gui_contains(g.chart, e->x, e->y)) {
            if (!u->treemap && gui_contains(u->center, e->x, e->y)) {
                gui_usage_command(ui, "usage.up");
                return 1;
            }
            int hit = gui_usage_chart_hit(ui, e->x, e->y);
            if (hit >= 0) {
                UsageNode *node = u->result->nodes + hit;
                if (!strcmp(node->row.kind, "directory"))
                    gui_usage_navigate(ui, node->row.path);
                else {
                    free(u->request.selection);
                    u->request.selection = xstrdup(node->row.path);
                    u->request.occurrence_page = 0;
                    u->details_scroll = 0;
                    u->help = 0;
                    gui_usage_request(ui);
                }
            }
            return 1;
        }
        return 1;
    }
    if (e->type == R_EVENT_MOUSE_WHEEL) {
        if (e->y < gui_pane_top(ui))
            return 0;
        int horizontal = (e->modifiers & R_MOD_SHIFT) || fabsf(e->wheel_x) > fabsf(e->wheel_y);
        float delta = horizontal && fabsf(e->wheel_x) > fabsf(e->wheel_y) ? e->wheel_x : e->wheel_y;
        if (gui_contains(g.details, e->x, e->y)) {
            if (horizontal)
                u->details_horizontal = (int)fmaxf(0, u->details_horizontal - delta * 60);
            else
                u->details_scroll = (int)fmaxf(0, u->details_scroll - delta * 3);
        } else if (gui_contains(g.tree, e->x, e->y)) {
            if (horizontal)
                u->horizontal = (int)fmaxf(0, u->horizontal - delta * 60);
            else
                u->offset = (int)fmaxf(0, u->offset - delta * 3);
        } else if (gui_contains(g.chart, e->x, e->y))
            gui_usage_command(ui, delta > 0 ? "usage.depth+" : "usage.depth-");
        return 1;
    }
    if (e->type != R_EVENT_KEY_DOWN)
        return e->type == R_EVENT_TEXT;
    int ctrl = !!(e->modifiers & R_MOD_CTRL), alt = !!(e->modifiers & R_MOD_ALT),
        shift = !!(e->modifiers & R_MOD_SHIFT);
    RKey key = e->key;
    if (key == R_KEY_F5 || key == R_KEY_F6 || key == R_KEY_F7 || key == R_KEY_F8 ||
        key == R_KEY_DELETE) {
        gui_notice(ui,
                   "Use Reveal in Curation (F3) before copying or changing curation decisions.");
        return 1;
    }
    /* Global display controls remain shared. Curation mutations stay scoped to
     * their visible workspace rather than acting on a hidden old selection. */
    if (key == R_KEY_F2 || key == R_KEY_F9 || key == R_KEY_F10 ||
        (ctrl && (key == R_KEY_EQUAL || key == R_KEY_MINUS || key == R_KEY_KP_ADD ||
                  key == R_KEY_KP_SUBTRACT || key == R_KEY_0 || key == R_KEY_COMMA)) ||
        (ctrl && alt && (key == R_KEY_F || key == R_KEY_T)) || (ctrl && key == R_KEY_L && !alt))
        return 0;
    if (key == R_KEY_F1) {
        gui_usage_command(ui, "help");
        return 1;
    }
    if (key == R_KEY_ESCAPE) {
        if (u->help)
            u->help = 0;
        else if (u->status.phase == USAGE_BUILDING)
            usage_cancel(u->service);
        return 1;
    }
    if (ctrl && key == R_KEY_F) {
        gui_usage_command(ui, "usage.scope");
        return 1;
    }
    if (ctrl && alt && key == R_KEY_C) {
        gui_usage_command(ui, "usage.columns");
        return 1;
    }
    if (ctrl && alt && key == R_KEY_W) {
        gui_usage_command(ui, "usage.fit");
        return 1;
    }
    if (ctrl && alt && key == R_KEY_L) {
        gui_usage_command(ui, "usage.layout");
        return 1;
    }
    if (ctrl && key == R_KEY_S) {
        gui_usage_command(ui, shift ? "usage.reverse" : "usage.sort");
        return 1;
    }
    if (ctrl && key == R_KEY_D) {
        u->focus = 1;
        ui->focus = 3;
        return 1;
    }
    if (key == R_KEY_F3 || (ctrl && key == R_KEY_ENTER)) {
        gui_usage_command(ui, "usage.reveal");
        return 1;
    }
    if (ctrl && key == R_KEY_HOME) {
        gui_usage_command(ui, "usage.root");
        return 1;
    }
    if (ctrl && (key == R_KEY_PAGE_UP || key == R_KEY_PAGE_DOWN)) {
        gui_usage_command(ui,
                          alt ? (key == R_KEY_PAGE_UP ? "usage.occurrences-" : "usage.occurrences+")
                              : (key == R_KEY_PAGE_UP ? "usage.previous" : "usage.next"));
        return 1;
    }
    if (key == R_KEY_TAB) {
        u->focus = !u->focus;
        ui->focus = u->focus ? 3 : 1;
        return 1;
    }
    if (key == R_KEY_LEFT_BRACKET || key == R_KEY_RIGHT_BRACKET) {
        gui_usage_command(ui, key == R_KEY_LEFT_BRACKET ? "usage.depth-" : "usage.depth+");
        return 1;
    }
    if (!ctrl && key == R_KEY_R) {
        gui_usage_command(ui, "usage.rings");
        return 1;
    }
    if (!ctrl && key == R_KEY_T) {
        gui_usage_command(ui, "usage.treemap");
        return 1;
    }
    if (key == R_KEY_BACKSPACE || (alt && key == R_KEY_LEFT)) {
        gui_usage_command(ui, "usage.up");
        return 1;
    }
    if (u->focus == 1) {
        int page = (int)fmaxf(1, (g.details.h - 56) / (ui->font_size + 5));
        if (key == R_KEY_UP)
            u->details_scroll--;
        else if (key == R_KEY_DOWN)
            u->details_scroll++;
        else if (key == R_KEY_PAGE_UP)
            u->details_scroll -= page;
        else if (key == R_KEY_PAGE_DOWN)
            u->details_scroll += page;
        else if (key == R_KEY_HOME)
            u->details_scroll = 0;
        else if (key == R_KEY_END)
            u->details_scroll = u->details_lines;
        else if (key == R_KEY_LEFT)
            u->details_horizontal -= 60;
        else if (key == R_KEY_RIGHT)
            u->details_horizontal += 60;
        u->details_scroll = u->details_scroll < 0 ? 0 : u->details_scroll;
        u->details_horizontal = u->details_horizontal < 0 ? 0 : u->details_horizontal;
        return 1;
    }
    if (!current)
        return 1;
    if (key == R_KEY_ENTER || key == R_KEY_KP_ENTER) {
        open_row(ui);
        return 1;
    }
    if ((key == R_KEY_SPACE || key == R_KEY_RIGHT) && u->selected >= 0) {
        UsageRow *row = u->result->rows + u->selected;
        if (key == R_KEY_RIGHT && is_expanded(u, row->path) && u->selected + 1 < u->result->count)
            gui_usage_select(ui, u->selected + 1);
        else
            gui_usage_expand(ui, u->selected);
        return 1;
    }
    if (key == R_KEY_LEFT) {
        if (u->selected >= 0 && is_expanded(u, u->result->rows[u->selected].path))
            gui_usage_expand(ui, u->selected);
        else if (u->selected >= 0 && u->result->rows[u->selected].depth > 0) {
            for (int i = u->selected - 1; i >= 0; i--)
                if (u->result->rows[i].depth < u->result->rows[u->selected].depth) {
                    gui_usage_select(ui, i);
                    break;
                }
        } else
            gui_usage_command(ui, "usage.up");
        return 1;
    }
    int selected = u->selected;
    if (key == R_KEY_DOWN)
        selected++;
    else if (key == R_KEY_UP)
        selected--;
    else if (key == R_KEY_PAGE_DOWN)
        selected += g.visible;
    else if (key == R_KEY_PAGE_UP)
        selected -= g.visible;
    else if (key == R_KEY_HOME)
        selected = 0;
    else if (key == R_KEY_END)
        selected = u->result->count - 1;
    else
        return 1;
    if (selected < 0)
        selected = 0;
    if (selected >= u->result->count)
        selected = u->result->count - 1;
    gui_usage_select(ui, selected);
    return 1;
}
