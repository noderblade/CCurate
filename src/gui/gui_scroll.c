#include "gui_scroll.h"
#include "gui_browser.h"
#include "gui_state.h"
#include "gui_theme.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Canvas scrollbars share geometry between painting and hit testing. Large file
 * lists use global row positions; only the requested page is loaded by the worker. */
void gui_scroll_begin(Gui *ui)
{
    for (int i = 0; i < GUI_SCROLLBARS; i++)
        ui->scrollbars[i].active = 0;
}

void gui_scrollbar_paint(Gui *ui, RPainter *p, int id, GuiRect track, double total, double visible,
                         double value, int horizontal)
{
    if (track.w <= 0 || track.h <= 0)
        return;
    GuiScrollbar *bar = &ui->scrollbars[id];
    double length = horizontal ? track.w : track.h;
    double maximum = fmax(0, total - visible);
    double thumb = maximum > 0 ? fmin(length, fmax(18, length * visible / fmax(1, total))) : length;
    value = fmax(0, fmin(maximum, value));
    double start = maximum > 0 ? (length - thumb) * value / maximum : 0;
    *bar = (GuiScrollbar){.track = track,
                          .thumb = track,
                          .value = value,
                          .maximum = maximum,
                          .page = fmax(1, visible),
                          .active = 1,
                          .horizontal = horizontal};
    if (horizontal) {
        bar->thumb.x += start;
        bar->thumb.w = thumb;
    } else {
        bar->thumb.y += start;
        bar->thumb.h = thumb;
    }
    Rdraw_rect(p, track.x, track.y, track.w, track.h, 4, GUI_COLOR(ui, raised), 1);
    Rdraw_rect(p, bar->thumb.x, bar->thumb.y, bar->thumb.w, bar->thumb.h, 4,
               GUI_HEX_COLOR(ui->scroll_drag == id ? gui_theme_current(ui)->accent
                             : maximum > 0         ? gui_theme_current(ui)->muted
                                                   : gui_theme_current(ui)->border),
               1);
}

static void apply(Gui *ui, int id, double value)
{
    GuiScrollbar *bar = &ui->scrollbars[id];
    value = fmax(0, fmin(bar->maximum, value));
    bar->value = value;
    if (id < 4) {
        int side = id / 2;
        ui->focus = side;
        if (id % 2)
            ui->pane[side].horizontal = (int)lround(value);
        else
            gui_seek_row(ui, side, (int64_t)llround(value));
    } else if (id < 8) {
        int side = id / 2;
        ui->focus = side;
        int *offset = side == 2 ? (id % 2 ? &ui->preview_horizontal : &ui->preview_scroll)
                                : (id % 2 ? &ui->details_horizontal : &ui->details_scroll);
        *offset = (int)lround(value);
    } else if (id < 10) {
        ui->focus = 4;
        if (id % 2)
            ui->log_horizontal = (int)lround(value);
        else {
            ui->log_follow = 0;
            ui->log_scroll = (int)lround(value);
        }
    } else if (id == 10)
        ui->font_scroll = (int)lround(value);
    else if (id == 11)
        ui->font_horizontal = (int)lround(value);
    else if (id == 12)
        ui->menu_offset = (int)lround(value);
    else
        ui->dropdown_offset = (int)lround(value);
    Rwidget_invalidate(ui->canvas);
}

static int allowed(Gui *ui, int id)
{
    if (ui->font_dialog)
        return id == 10 || id == 11;
    if (ui->dropdown)
        return id == 13;
    if (ui->menu)
        return id == 12;
    return id < 10;
}

int gui_scroll_event(Gui *ui, const REvent *e)
{
    if (ui->scroll_drag >= 0) {
        int id = ui->scroll_drag;
        GuiScrollbar *bar = &ui->scrollbars[id];
        if (e->type == R_EVENT_MOUSE_UP && e->button == R_MOUSE_LEFT) {
            activity_log(ui->c, LOG_DEBUG, "view.scroll", NULL, NULL,
                         "Scrollbar %d: position %.0f / %.0f", id, bar->value, bar->maximum);
            ui->scroll_drag = -1;
            return 1;
        }
        if (e->type == R_EVENT_MOUSE_MOVE) {
            float at = bar->horizontal ? e->x : e->y;
            float start = bar->horizontal ? bar->track.x : bar->track.y;
            float length = bar->horizontal ? bar->track.w : bar->track.h;
            float thumb = bar->horizontal ? bar->thumb.w : bar->thumb.h;
            apply(ui, id, (at - start - ui->scroll_grab) * bar->maximum / fmaxf(1, length - thumb));
            return 1;
        }
    }
    if (e->type != R_EVENT_MOUSE_DOWN || e->button != R_MOUSE_LEFT)
        return 0;
    for (int id = 0; id < GUI_SCROLLBARS; id++) {
        GuiScrollbar *bar = &ui->scrollbars[id];
        if (!allowed(ui, id) || !bar->active || !gui_contains(bar->track, e->x, e->y))
            continue;
        if (bar->maximum > 0) {
            float at = bar->horizontal ? e->x : e->y;
            float start = bar->horizontal ? bar->thumb.x : bar->thumb.y;
            if (gui_contains(bar->thumb, e->x, e->y)) {
                ui->scroll_drag = id;
                ui->scroll_grab = at - start;
            } else
                apply(ui, id, bar->value + (at < start ? -bar->page : bar->page));
        }
        Rwidget_focus(ui->canvas);
        return 1;
    }
    return 0;
}

void gui_text_cache_free(GuiTextCache *cache)
{
    free(cache->source);
    free(cache->buffer);
    free(cache->lines);
    memset(cache, 0, sizeof(*cache));
}

static void text_cache(Gui *ui, GuiTextCache *cache, const char *text)
{
    text = text ? text : "";
    if (cache->source && cache->font == ui->font && cache->size == ui->font_size &&
        !strcmp(cache->source, text))
        return;
    gui_text_cache_free(cache);
    cache->font = ui->font;
    cache->size = ui->font_size;
    cache->source = xstrdup(text);
    cache->buffer = xstrdup(text);
    int lines = 1;
    for (const char *s = text; *s; s++)
        lines += *s == '\n';
    cache->lines = xmalloc((size_t)lines * sizeof(char *));
    char *line = cache->buffer;
    do {
        char *end = strchr(line, '\n');
        if (end)
            *end = 0;
        cache->lines[cache->count++] = line;
        float width = 0;
        Rfont_measure_text(ui->font, ui->font_size, line, &width, NULL);
        cache->width = fmaxf(cache->width, width);
        line = end ? end + 1 : NULL;
    } while (line);
}

void gui_text_scroll_clamp(Gui *ui, int side)
{
    int *vertical = side == 2 ? &ui->preview_scroll : &ui->details_scroll;
    int *horizontal = side == 2 ? &ui->preview_horizontal : &ui->details_horizontal;
    *vertical = (int)fmax(0, fmin(ui->scrollbars[side * 2].maximum, *vertical));
    *horizontal = (int)fmax(0, fmin(ui->scrollbars[side * 2 + 1].maximum, *horizontal));
}

void gui_text_panel_paint(Gui *ui, RPainter *p, int side, GuiRect body, const char *text)
{
    GuiTextCache *cache = &ui->text_cache[side - 2];
    text_cache(ui, cache, text);
    int *vertical = side == 2 ? &ui->preview_scroll : &ui->details_scroll;
    int *horizontal = side == 2 ? &ui->preview_horizontal : &ui->details_horizontal;
    float lh = ui->font_size + 5;
    int visible = (int)fmaxf(1, body.h / lh);
    *vertical = (int)fmax(0, fmin(fmax(0, cache->count - visible), *vertical));
    *horizontal = (int)fmax(0, fmin(fmax(0, cache->width - body.w), *horizontal));
    gui_scrollbar_paint(ui, p, side * 2, (GuiRect){body.x + body.w + 3, body.y, 10, body.h},
                        cache->count, visible, *vertical, 0);
    gui_scrollbar_paint(ui, p, side * 2 + 1, (GuiRect){body.x, body.y + body.h + 3, body.w, 10},
                        cache->width, body.w, *horizontal, 1);
    Rdraw_push_clip_rect(p, body.x, body.y, body.w, body.h);
    for (int i = *vertical; i < cache->count && i < *vertical + visible; i++)
        Rdraw_text(p, ui->font, ui->font_size, body.x - *horizontal, body.y + (i - *vertical) * lh,
                   cache->lines[i], GUI_COLOR(ui, text), 1);
    Rdraw_pop_clip_rect(p);
}
