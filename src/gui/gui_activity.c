#include "gui_activity.h"
#include "gui_choices.h"
#include "gui_controller.h"
#include "gui_layout.h"
#include "gui_render.h"
#include "gui_scroll.h"
#include "gui_state.h"
#include "gui_theme.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Activity panel rendering, scrolling and severity choices. */
/* Canvas controls share the main event loop; no filesystem IO is done here. */
static const char *log_levels[] = {"All", "Info", "Warnings", "Errors", "Debug"};
static const int log_level_ids[] = {-1, LOG_INFO, LOG_WARNING, LOG_ERROR, LOG_DEBUG};

static int gui_log_rows(Gui *ui, GuiGeometry g);
static void gui_log_clamp(Gui *ui);

int gui_log_level_index(Gui *ui)
{
    for (int i = 0; i < 5; i++)
        if (log_level_ids[i] == ui->log_level)
            return i;
    return 0;
}

static int gui_log_rows(Gui *ui, GuiGeometry g)
{
    return (int)fmaxf(1, (g.pane[4].h - 82) / ui->row_height);
}

static void gui_log_clamp(Gui *ui)
{
    int last = ui->log_count - gui_log_rows(ui, gui_geometry(ui));
    if (last < 0)
        last = 0;
    if (ui->log_follow || ui->log_scroll > last)
        ui->log_scroll = last;
    if (ui->log_scroll < 0)
        ui->log_scroll = 0;
    if (ui->log_horizontal < 0)
        ui->log_horizontal = 0;
}

void gui_refresh_log(Gui *ui)
{
    if (activity_status(ui->c->log, ui->log_error) == ui->log_revision) {
        gui_log_clamp(ui);
        return;
    }
    int count;
    ActivityLine *lines =
        activity_snapshot(ui->c->log, ui->log_level, &count, &ui->log_revision, ui->log_error);
    activity_lines_free(ui->log_lines, ui->log_count);
    ui->log_lines = lines;
    ui->log_count = count;
    gui_log_clamp(ui);
}

GuiRect gui_log_button(GuiGeometry g, int index)
{
    GuiRect r = g.pane[4];
    if (index == 0)
        return (GuiRect){r.x + r.w - 330, r.y + 6, 145, 28};
    if (index == 1)
        return (GuiRect){r.x + r.w - 179, r.y + 6, 115, 28};
    return (GuiRect){r.x + r.w - 58, r.y + 6, 50, 28};
}

void gui_log_paint(Gui *ui, RPainter *p, GuiGeometry g)
{
    GuiRect r = g.pane[4];
    gui_log_clamp(ui);
    Rdraw_rect(p, r.x, r.y, r.w, r.h, 8, GUI_COLOR(ui, panel), 1);
    Rdraw_rect_outline(p, r.x, r.y, r.w, r.h, 8, ui->focus == 4 ? 2 : 1,
                       GUI_HEX_COLOR(ui->focus == 4 ? gui_theme_current(ui)->accent
                                                    : gui_theme_current(ui)->border),
                       1);
    Rdraw_push_clip_rect(p, r.x + 2, r.y + 2, fmaxf(0, r.w - 4), fmaxf(0, r.h - 4));
    gui_text_clip(ui, p, r.x + 12, r.y + 12, r.w - 352, 24, "ACTIVITY LOG", GUI_COLOR(ui, text),
                  ui->font_size);
    char level[48];
    snprintf(level, sizeof(level), "Level: %s v", log_levels[gui_log_level_index(ui)]);
    const char *labels[] = {level, ui->log_follow ? "Follow: On" : "Follow: Off", "Hide"};
    for (int i = 0; i < 3; i++) {
        GuiRect b = gui_log_button(g, i);
        Rdraw_rect(p, b.x, b.y, b.w, b.h, 4, GUI_COLOR(ui, raised), 1);
        gui_text_clip(ui, p, b.x + 8, b.y + 6, b.w - 12, 22, labels[i], GUI_COLOR(ui, text),
                      ui->font_size - 1);
    }
    char caption[512];
    if (*ui->log_error)
        snprintf(caption, sizeof(caption), "%s", ui->log_error);
    else
        snprintf(caption, sizeof(caption),
                 "%d matching recent entries | .ccurate/activity.log | Left/Right: pan",
                 ui->log_count);
    gui_text_clip(
        ui, p, r.x + 12, r.y + 39, r.w - 24, 20, caption,
        GUI_HEX_COLOR(*ui->log_error ? gui_theme_current(ui)->error : gui_theme_current(ui)->muted),
        ui->font_size - 2);
    float cell = 0, content_width = 0;
    Rfont_measure_text(ui->font, ui->font_size - 1, "M", &cell, NULL);
    for (int i = 0; i < ui->log_count; i++) {
        int chars = 0;
        for (const unsigned char *ch = (const unsigned char *)ui->log_lines[i].text; *ch; ch++)
            chars += (*ch & 0xc0) != 0x80;
        content_width = fmaxf(content_width, chars * cell);
    }
    float view_width = fmaxf(1, r.w - 32), view_height = fmaxf(1, r.h - 82);
    ui->log_horizontal =
        (int)fmaxf(0, fminf(fmaxf(0, content_width - view_width), ui->log_horizontal));
    gui_scrollbar_paint(ui, p, 8, (GuiRect){r.x + r.w - 17, r.y + 62, 10, view_height},
                        ui->log_count, gui_log_rows(ui, g), ui->log_scroll, 0);
    gui_scrollbar_paint(ui, p, 9, (GuiRect){r.x + 12, r.y + r.h - 17, view_width, 10},
                        content_width, view_width, ui->log_horizontal, 1);
    Rdraw_push_clip_rect(p, r.x + 12, r.y + 62, view_width, view_height);
    for (int i = ui->log_scroll; i < ui->log_count && i < ui->log_scroll + gui_log_rows(ui, g);
         i++) {
        ActivityLine *line = &ui->log_lines[i];
        unsigned color = line->level == LOG_ERROR     ? gui_theme_current(ui)->error
                         : line->level == LOG_WARNING ? gui_theme_current(ui)->warning
                         : line->level == LOG_DEBUG   ? gui_theme_current(ui)->muted
                                                      : gui_theme_current(ui)->text;
        const char *s = line->text;
        Rdraw_text(p, ui->font, ui->font_size - 1, r.x + 12 - ui->log_horizontal,
                   r.y + 63 + (i - ui->log_scroll) * ui->row_height, s, GUI_HEX_COLOR(color), 1);
    }
    if (!ui->log_count)
        Rdraw_text(p, ui->font, ui->font_size - 1, r.x + 12, r.y + 63,
                   "No recent entries at this level. The full history is in activity.log.",
                   GUI_COLOR(ui, muted), 1);
    Rdraw_pop_clip_rect(p);
    Rdraw_pop_clip_rect(p);
}

int gui_log_event(Gui *ui, const REvent *e, GuiGeometry g)
{
    if (!ui->log_visible || ui->drag)
        return 0;
    if (e->type == R_EVENT_MOUSE_DOWN && e->button == R_MOUSE_LEFT &&
        gui_contains(g.pane[4], e->x, e->y)) {
        ui->focus = 4;
        Rwidget_focus(ui->canvas);
        if (gui_contains(gui_log_button(g, 0), e->x, e->y))
            gui_dropdown_open(ui, 1);
        else if (gui_contains(gui_log_button(g, 1), e->x, e->y))
            gui_command(ui, "logfollow");
        else if (gui_contains(gui_log_button(g, 2), e->x, e->y))
            gui_command(ui, "log");
        return 1;
    }
    if (e->type == R_EVENT_MOUSE_WHEEL && gui_contains(g.pane[4], e->x, e->y)) {
        int delta = (int)roundf(e->wheel_y * 3);
        if (e->modifiers & R_MOD_SHIFT)
            ui->log_horizontal -= delta * 24;
        else {
            ui->log_scroll -= delta;
            ui->log_follow = 0;
        }
        ui->focus = 4;
        gui_log_clamp(ui);
        return 1;
    }
    if (ui->focus != 4 || e->type != R_EVENT_KEY_DOWN || (e->modifiers & (R_MOD_CTRL | R_MOD_ALT)))
        return 0;
    int rows = gui_log_rows(ui, g);
    switch (e->key) {
    case R_KEY_UP:
        ui->log_scroll--;
        ui->log_follow = 0;
        break;
    case R_KEY_DOWN:
        ui->log_scroll++;
        ui->log_follow = 0;
        break;
    case R_KEY_PAGE_UP:
        ui->log_scroll -= rows;
        ui->log_follow = 0;
        break;
    case R_KEY_PAGE_DOWN:
        ui->log_scroll += rows;
        ui->log_follow = 0;
        break;
    case R_KEY_HOME:
        ui->log_scroll = 0;
        ui->log_horizontal = 0;
        ui->log_follow = 0;
        break;
    case R_KEY_END:
        ui->log_follow = 1;
        break;
    case R_KEY_LEFT:
        ui->log_horizontal -= 64;
        break;
    case R_KEY_RIGHT:
        ui->log_horizontal += 64;
        break;
    default:
        return 0;
    }
    gui_log_clamp(ui);
    return 1;
}

const char *gui_activity_level_name(int index)
{
    return log_levels[index >= 0 && index < 5 ? index : 0];
}

int gui_activity_level_id(int index)
{
    return log_level_ids[index >= 0 && index < 5 ? index : 0];
}
