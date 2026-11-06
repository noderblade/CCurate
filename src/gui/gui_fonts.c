#include "gui_fonts.h"
#include "gui_controller.h"
#include "gui_layout.h"
#include "gui_render.h"
#include "gui_scroll.h"
#include "gui_state.h"
#include "gui_theme.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Installed monospace font chooser and font resource lifetime. */
typedef struct {
    GuiRect box, list, sample, buttons[3];
    int rows;
} FontLayout;

static int gui_builtin_font(Rid id);
static const MonoFont *gui_font_face(Gui *ui, int row);
static const char *gui_font_label(Gui *ui, int row);
static void gui_font_release_sample(Gui *ui);
static void gui_font_set_active(Gui *ui, Rid font);
static void gui_font_request_row(Gui *ui, int row, int mode);
static int gui_font_saved_row(Gui *ui);
static void gui_font_picker_close(Gui *ui);
static void gui_font_apply(Gui *ui);
static void gui_font_matches(Gui *ui, int prefer);
static void gui_font_refresh(Gui *ui);
static FontLayout gui_font_layout(Gui *ui);
static void gui_font_move(Gui *ui, int row);
static void gui_font_add_filter(Gui *ui, uint32_t code);

static int gui_builtin_font(Rid id)
{
    return id == R_INVALID_ID || id == Rfont_get_monospace();
}

static const MonoFont *gui_font_face(Gui *ui, int row)
{
    if (!ui->fonts || row == 1)
        return NULL;
    int index = row == 0 ? ui->fonts->default_index : row - 2;
    return index >= 0 && index < ui->fonts->count ? &ui->fonts->fonts[index] : NULL;
}

static const char *gui_font_label(Gui *ui, int row)
{
    if (row == 0)
        return "System monospace (automatic)";
    if (row == 1)
        return "Bundled Roboto Mono";
    const MonoFont *f = gui_font_face(ui, row);
    return f ? f->label : "Font unavailable";
}

static void gui_font_release_sample(Gui *ui)
{
    if (!gui_builtin_font(ui->font_sample) && ui->font_sample != ui->font)
        Rfont_destroy(ui->font_sample);
    ui->font_sample = R_INVALID_ID;
}

static void gui_font_set_active(Gui *ui, Rid font)
{
    Rid previous = ui->font;
    ui->font = font;
    Rtheme_set_font(font); /* Labels, buttons, text fields, tooltips and canvas. */
    Rwidget_set_font(ui->overlay, font);
    if (previous != font && !gui_builtin_font(previous))
        Rfont_destroy(previous);
    Rwidget_invalidate(ui->canvas);
}

static void gui_font_request_row(Gui *ui, int row, int mode)
{
    gui_font_release_sample(ui);
    ui->font_serial = 0;
    ui->font_apply_pending = 0;
    ui->font_error[0] = 0;
    ui->font_load_row = row;
    ui->font_load_mode = mode;
    const MonoFont *f = gui_font_face(ui, row);
    if (f)
        ui->font_serial = font_service_request(ui->font_service, f->file, f->index);
    else {
        ui->font_sample = Rfont_get_monospace();
        if (!mode) {
            gui_font_set_active(ui, ui->font_sample);
            ui->font_sample = R_INVALID_ID;
        }
    }
}

static int gui_font_saved_row(Gui *ui)
{
    if (!*ui->font_file)
        return 0;
    if (!strcmp(ui->font_file, "@bundled"))
        return 1;
    int family_match = -1;
    if (ui->fonts)
        for (int i = 0; i < ui->fonts->count; i++) {
            const MonoFont *f = &ui->fonts->fonts[i];
            if (!strcmp(f->file, ui->font_file) && f->index == ui->font_index)
                return i + 2;
            if (!strcmp(f->family, ui->font_family) && !strcmp(f->style, ui->font_style))
                family_match = i + 2;
        }
    return family_match >= 0 ? family_match : 0;
}

static void gui_font_picker_close(Gui *ui)
{
    ui->font_dialog = 0;
    ui->font_serial = 0;
    ui->font_apply_pending = 0;
    gui_font_release_sample(ui);
    ui->focus = ui->font_previous_focus;
    Rwidget_focus(ui->canvas);
    Rwidget_invalidate(ui->canvas);
}

static void gui_font_apply(Gui *ui)
{
    if (ui->font_sample == R_INVALID_ID) {
        if (ui->font_serial && !*ui->font_error)
            ui->font_apply_pending = 1;
        return;
    }
    const MonoFont *f = gui_font_face(ui, ui->font_candidate);
    free(ui->font_file);
    free(ui->font_family);
    free(ui->font_style);
    ui->font_file = xstrdup(ui->font_candidate == 0   ? ""
                            : ui->font_candidate == 1 ? "@bundled"
                                                      : f->file);
    ui->font_family = xstrdup(f && ui->font_candidate > 1 ? f->family : "");
    ui->font_style = xstrdup(f && ui->font_candidate > 1 ? f->style : "");
    ui->font_index = f && ui->font_candidate > 1 ? f->index : 0;
    gui_font_set_active(ui, ui->font_sample);
    ui->font_sample = R_INVALID_ID;
    char *message = fmt("Monospace font: %s", gui_font_label(ui, ui->font_candidate));
    gui_notice(ui, message);
    free(message);
    gui_font_picker_close(ui);
    gui_save_ui(ui);
}

static void gui_font_matches(Gui *ui, int prefer)
{
    free(ui->font_matches);
    int total = 2 + (ui->fonts ? ui->fonts->count : 0);
    ui->font_matches = xmalloc((size_t)total * sizeof(int));
    ui->font_match_count = 0;
    ui->font_match_row = 0;
    for (int i = 0; i < total; i++)
        if (!*ui->font_filter || strcasestr(gui_font_label(ui, i), ui->font_filter)) {
            if (i == prefer)
                ui->font_match_row = ui->font_match_count;
            ui->font_matches[ui->font_match_count++] = i;
        }
    ui->font_scroll = 0;
    ui->font_candidate = ui->font_match_count ? ui->font_matches[ui->font_match_row] : -1;
    if (ui->font_candidate >= 0)
        gui_font_request_row(ui, ui->font_candidate, 1);
    else {
        gui_font_release_sample(ui);
        ui->font_serial = 0;
        ui->font_error[0] = 0;
    }
}

void gui_font_picker_open(Gui *ui)
{
    ui->menu = ui->dropdown = 0;
    if (!ui->font_dialog)
        ui->font_previous_focus = ui->focus;
    ui->font_dialog = 1;
    ui->font_control = 0;
    ui->font_filter[0] = 0;
    gui_font_matches(ui, gui_font_saved_row(ui));
    Rwidget_set_tooltip(ui->canvas, "");
    Rwidget_focus(ui->canvas);
    Rwidget_invalidate(ui->canvas);
}

static void gui_font_refresh(Gui *ui)
{
    ui->font_serial = 0;
    ui->font_apply_pending = 0;
    font_service_reload(ui->font_service);
}

void gui_font_tick(Gui *ui)
{
    MonoFonts *list = font_service_take_list(ui->font_service);
    if (list) {
        mono_fonts_free(ui->fonts);
        ui->fonts = list;
        int row = gui_font_saved_row(ui);
        if (ui->font_dialog)
            gui_font_matches(ui, row);
        else {
            if (*ui->font_file && strcmp(ui->font_file, "@bundled") && !row)
                gui_notice(ui,
                           "Saved font is unavailable; using system monospace. Choose another in "
                           "Layout > Monospace font.");
            gui_font_request_row(ui, row, 0);
        }
    }
    FontBytes *data = font_service_take_bytes(ui->font_service);
    if (!data)
        return;
    if (data->serial != ui->font_serial) {
        font_bytes_free(data);
        return;
    }
    ui->font_serial = 0;
    Rid font = R_INVALID_ID;
    if (*data->error)
        snprintf(ui->font_error, sizeof(ui->font_error), "%s", data->error);
    else {
        font = Rfont_create((const char *)data->bytes, data->size);
        if (font == R_INVALID_ID)
            snprintf(ui->font_error, sizeof(ui->font_error),
                     "Raven cannot render this font. Choose another face.");
        else {
            float reference = 0;
            Rfont_measure_text(font, 14, "M", &reference, NULL);
            for (int ch = 32; ch < 127; ch++) {
                char glyph[2] = {(char)ch, 0};
                float width = 0;
                Rfont_measure_text(font, 14, glyph, &width, NULL);
                if (reference <= 0 || fabsf(width - reference) > .04f) {
                    Rfont_destroy(font);
                    font = R_INVALID_ID;
                    snprintf(ui->font_error, sizeof(ui->font_error),
                             "This face does not render with fixed ASCII widths. Choose a "
                             "monospace variant.");
                    break;
                }
            }
            if (font != R_INVALID_ID)
                Rfont_set_fallback(font, Rfont_get_monospace());
        }
    }
    font_bytes_free(data);
    if (font == R_INVALID_ID) {
        ui->font_apply_pending = 0;
        if (!ui->font_load_mode)
            gui_notice(ui, ui->font_error);
        return;
    }
    ui->font_sample = font;
    if (!ui->font_load_mode) {
        gui_font_set_active(ui, font);
        ui->font_sample = R_INVALID_ID;
    } else if (ui->font_dialog && ui->font_apply_pending)
        gui_font_apply(ui);
}

static FontLayout gui_font_layout(Gui *ui)
{
    FontLayout g = {0};
    float w = fminf(860, ui->width - 24), h = fminf(660, ui->height - 60);
    g.box = (GuiRect){(ui->width - w) / 2, (ui->height - h) / 2, w, h};
    GuiRect r = g.box;
    float sample = h > 400 ? 112 : h > 290 ? 78 : 64;
    g.list = (GuiRect){r.x + 12, r.y + 82, w - 24, fmaxf(28, h - 82 - sample - 62)};
    g.list.h = fmaxf(28, g.list.h - 14);
    g.rows = (int)fmaxf(1, g.list.h / 28);
    g.sample = (GuiRect){r.x + 14, r.y + h - 50 - sample, w - 28, sample - 6};
    float button = fminf(145, (w - 48) / 3);
    for (int i = 0; i < 3; i++)
        g.buttons[i] = (GuiRect){r.x + 14 + i * (button + 10), r.y + h - 40, button, 28};
    return g;
}

void gui_font_picker_paint(Gui *ui, RPainter *p)
{
    if (!ui->font_dialog)
        return;
    FontLayout g = gui_font_layout(ui);
    GuiRect r = g.box;
    Rdraw_rect(p, 0, 0, ui->width, ui->height, 0, 0, 0, 0, .45f);
    Rdraw_rect(p, r.x, r.y, r.w, r.h, 8, GUI_COLOR(ui, panel), 1);
    Rdraw_rect_outline(p, r.x, r.y, r.w, r.h, 8, 2, GUI_COLOR(ui, accent), 1);
    gui_text_clip(ui, p, r.x + 14, r.y + 13, r.w - 28, 24, "MONOSPACE FONT", GUI_COLOR(ui, text),
                  16);
    char *info = fmt("Filter: %s%s", *ui->font_filter ? ui->font_filter : "",
                     *ui->font_filter ? "" : "Type to narrow the list; Ctrl+U clears");
    gui_text_clip(ui, p, r.x + 14, r.y + 43, r.w - 28, 24, info, GUI_COLOR(ui, muted), 13);
    free(info);
    int last = ui->font_match_count - g.rows;
    ui->font_scroll = (int)fmaxf(0, fminf(fmaxf(0, last), ui->font_scroll));
    float cell = 0, content_width = 0;
    Rfont_measure_text(ui->font, 14, "M", &cell, NULL);
    for (int i = 0; i < ui->font_match_count; i++)
        content_width =
            fmaxf(content_width, strlen(gui_font_label(ui, ui->font_matches[i])) * cell + 20);
    float view_width = fmaxf(1, g.list.w - 18);
    ui->font_horizontal =
        (int)fmaxf(0, fminf(fmaxf(0, content_width - view_width), ui->font_horizontal));
    Rdraw_push_clip_rect(p, g.list.x, g.list.y, view_width, g.list.h);
    for (int at = ui->font_scroll; at < ui->font_match_count && at < ui->font_scroll + g.rows;
         at++) {
        float y = g.list.y + (at - ui->font_scroll) * 28;
        if (at == ui->font_match_row)
            Rdraw_rect(p, g.list.x, y, g.list.w - 8, 27, 4, GUI_COLOR(ui, selection), 1);
        gui_text_clip(ui, p, g.list.x + 9 - ui->font_horizontal, y + 5,
                      fmaxf(content_width, view_width), 23,
                      gui_font_label(ui, ui->font_matches[at]), GUI_COLOR(ui, text), 14);
    }
    if (!ui->font_match_count)
        gui_text_clip(ui, p, g.list.x + 9, g.list.y + 5, g.list.w - 18, 24,
                      "No matching fonts. Ctrl+U clears the filter.", GUI_COLOR(ui, muted), 13);
    Rdraw_pop_clip_rect(p);
    gui_scrollbar_paint(ui, p, 10, (GuiRect){g.list.x + g.list.w - 13, g.list.y, 10, g.list.h},
                        ui->font_match_count, g.rows, ui->font_scroll, 0);
    gui_scrollbar_paint(ui, p, 11, (GuiRect){g.list.x, g.list.y + g.list.h + 2, view_width, 10},
                        content_width, view_width, ui->font_horizontal, 1);
    const MonoFont *face = gui_font_face(ui, ui->font_candidate);
    const char *status = *ui->font_error                          ? ui->font_error
                         : ui->font_serial                        ? "Loading font preview..."
                         : font_service_loading(ui->font_service) ? "Discovering installed fonts..."
                         : face                                   ? face->label
                         : ui->font_candidate < 0                 ? "Select a font"
                                                                  : "Bundled Roboto Mono";
    gui_text_clip(ui, p, g.sample.x, g.sample.y, g.sample.w, 22, status,
                  GUI_HEX_COLOR(*ui->font_error ? gui_theme_current(ui)->error
                                                : gui_theme_current(ui)->accent),
                  12);
    if (ui->font_sample != R_INVALID_ID && g.sample.h > 35) {
        Rdraw_push_clip_rect(p, g.sample.x, g.sample.y + 24, g.sample.w, g.sample.h - 24);
        Rdraw_text(p, ui->font_sample, 16, g.sample.x, g.sample.y + 24,
                   "Il1  O0o  0123456789  AaBbCc  [] {} () <>", GUI_COLOR(ui, text), 1);
        if (g.sample.h > 70)
            Rdraw_text(p, ui->font_sample, 14, g.sample.x, g.sample.y + 49,
                       "Photos/2026/  OK D x3  2.31 GiB  0644", GUI_COLOR(ui, text), 1);
        Rdraw_pop_clip_rect(p);
    }
    const char *labels[] = {"Apply (Enter)", "Cancel (Esc)", "Refresh (F5)"};
    for (int i = 0; i < 3; i++) {
        GuiRect b = g.buttons[i];
        Rdraw_rect(p, b.x, b.y, b.w, b.h, 4, GUI_COLOR(ui, raised), 1);
        if (ui->font_control == i + 1)
            Rdraw_rect_outline(p, b.x, b.y, b.w, b.h, 4, 2, GUI_COLOR(ui, accent), 1);
        gui_text_clip(ui, p, b.x + 7, b.y + 6, b.w - 12, 22, labels[i], GUI_COLOR(ui, text), 12);
    }
}

static void gui_font_move(Gui *ui, int row)
{
    if (!ui->font_match_count)
        return;
    if (row < 0)
        row = 0;
    if (row >= ui->font_match_count)
        row = ui->font_match_count - 1;
    int visible = gui_font_layout(ui).rows;
    if (row < ui->font_scroll)
        ui->font_scroll = row;
    if (row >= ui->font_scroll + visible)
        ui->font_scroll = row - visible + 1;
    ui->font_control = 0;
    if (row != ui->font_match_row) {
        ui->font_match_row = row;
        ui->font_candidate = ui->font_matches[row];
        gui_font_request_row(ui, ui->font_candidate, 1);
    }
}

static void gui_font_add_filter(Gui *ui, uint32_t code)
{
    if (code < 32 || code == 127 || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff))
        return;
    unsigned char buf[4];
    size_t n = 0;
    if (code < 0x80)
        buf[n++] = code;
    else if (code < 0x800) {
        buf[n++] = 0xc0 | (code >> 6);
        buf[n++] = 0x80 | (code & 63);
    } else if (code < 0x10000) {
        buf[n++] = 0xe0 | (code >> 12);
        buf[n++] = 0x80 | ((code >> 6) & 63);
        buf[n++] = 0x80 | (code & 63);
    } else {
        buf[n++] = 0xf0 | (code >> 18);
        buf[n++] = 0x80 | ((code >> 12) & 63);
        buf[n++] = 0x80 | ((code >> 6) & 63);
        buf[n++] = 0x80 | (code & 63);
    }
    size_t len = strlen(ui->font_filter);
    if (len + n >= sizeof(ui->font_filter))
        return;
    memcpy(ui->font_filter + len, buf, n);
    ui->font_filter[len + n] = 0;
    ui->font_control = 0;
    gui_font_matches(ui, ui->font_candidate);
}

int gui_font_picker_event(Gui *ui, const REvent *e)
{
    if (!ui->font_dialog)
        return 0;
    FontLayout g = gui_font_layout(ui);
    if (e->type == R_EVENT_KEY_DOWN) {
        switch (e->key) {
        case R_KEY_ESCAPE:
            gui_font_picker_close(ui);
            break;
        case R_KEY_F10:
            Rquit(0);
            break;
        case R_KEY_F5:
            gui_font_refresh(ui);
            break;
        case R_KEY_UP:
            gui_font_move(ui, ui->font_match_row - 1);
            break;
        case R_KEY_DOWN:
            gui_font_move(ui, ui->font_match_row + 1);
            break;
        case R_KEY_PAGE_UP:
            gui_font_move(ui, ui->font_match_row - g.rows);
            break;
        case R_KEY_PAGE_DOWN:
            gui_font_move(ui, ui->font_match_row + g.rows);
            break;
        case R_KEY_HOME:
            gui_font_move(ui, 0);
            break;
        case R_KEY_END:
            gui_font_move(ui, ui->font_match_count - 1);
            break;
        case R_KEY_TAB:
            ui->font_control = (ui->font_control + ((e->modifiers & R_MOD_SHIFT) ? 3 : 1)) % 4;
            break;
        case R_KEY_ENTER:
        case R_KEY_KP_ENTER:
            if (ui->font_control == 2)
                gui_font_picker_close(ui);
            else if (ui->font_control == 3)
                gui_font_refresh(ui);
            else
                gui_font_apply(ui);
            break;
        case R_KEY_BACKSPACE: {
            size_t len = strlen(ui->font_filter);
            if (len) {
                do {
                    len--;
                } while (len && ((unsigned char)ui->font_filter[len] & 0xc0) == 0x80);
                ui->font_filter[len] = 0;
                gui_font_matches(ui, ui->font_candidate);
            }
            break;
        }
        case R_KEY_U:
            if (e->modifiers & R_MOD_CTRL) {
                ui->font_filter[0] = 0;
                gui_font_matches(ui, ui->font_candidate);
            }
            break;
        default:
            break;
        }
    } else if (e->type == R_EVENT_TEXT && !(e->modifiers & (R_MOD_CTRL | R_MOD_ALT)))
        gui_font_add_filter(ui, e->codepoint);
    else if (e->type == R_EVENT_MOUSE_WHEEL)
        gui_font_move(ui, ui->font_match_row + (e->wheel_y > 0 ? -3 : 3));
    else if (e->type == R_EVENT_MOUSE_DOWN && e->button == R_MOUSE_LEFT) {
        if (gui_contains(g.list, e->x, e->y)) {
            int row = ui->font_scroll + (int)((e->y - g.list.y) / 28);
            if (row < ui->font_match_count) {
                gui_font_move(ui, row);
                if (e->clicks >= 2)
                    gui_font_apply(ui);
            }
        } else
            for (int i = 0; i < 3; i++)
                if (gui_contains(g.buttons[i], e->x, e->y)) {
                    if (i == 0)
                        gui_font_apply(ui);
                    else if (i == 1)
                        gui_font_picker_close(ui);
                    else
                        gui_font_refresh(ui);
                    break;
                }
    }
    Rwidget_invalidate(ui->canvas);
    return 1;
}

void gui_font_shutdown(Gui *ui)
{
    font_service_stop(ui->font_service);
    ui->font_service = NULL;
    gui_font_release_sample(ui);
    mono_fonts_free(ui->fonts);
    ui->fonts = NULL;
    free(ui->font_file);
    free(ui->font_family);
    free(ui->font_style);
    free(ui->font_matches);
}
