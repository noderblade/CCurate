#include "gui_render.h"
#include "gui_activity.h"
#include "gui_browser.h"
#include "gui_choices.h"
#include "gui_columns.h"
#include "gui_fonts.h"
#include "gui_layout.h"
#include "gui_menu.h"
#include "gui_scroll.h"
#include "gui_state.h"
#include "gui_theme.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Canvas rendering and shared text drawing helpers. */

static void gui_pane_paint(Gui *ui, RPainter *p, int side, float x, float y, float w, float h);

void gui_text_clip(Gui *ui, RPainter *p, float x, float y, float w, float h, const char *s, float r,
                   float g, float b, float size)
{
    if (w <= 0 || h <= 0)
        return;
    Rdraw_push_clip_rect(p, x, y, w, h);
    Rdraw_text(p, ui->font, size, x, y, s, r, g, b, 1);
    Rdraw_pop_clip_rect(p);
}

void gui_multiline(Gui *ui, RPainter *p, const char *s, float x, float y, float w, float h,
                   int scroll)
{
    if (!s)
        return;
    Rdraw_push_clip_rect(p, x, y, w, h);
    float lh = ui->font_size + 5;
    float cell_width = 0;
    Rfont_measure_text(ui->font, ui->font_size, "M", &cell_width, NULL);
    int maxchars = (int)(w / fmaxf(1, cell_width));
    if (maxchars < 8)
        maxchars = 8;
    const char *ptr = s;
    int row = 0;
    while (*ptr && y + (row - scroll) * lh < y + h) {
        const char *end = strchr(ptr, '\n');
        size_t len = end ? (size_t)(end - ptr) : strlen(ptr);
        if (!len) {
            row++;
            ptr = end ? end + 1 : ptr + len;
            continue;
        }
        size_t pos = 0;
        while (pos < len) {
            size_t take = len - pos;
            if (take > (size_t)maxchars)
                take = maxchars; /* Avoid splitting a UTF-8 continuation sequence. */
            while (take && pos + take < len && ((unsigned char)ptr[pos + take] & 0xc0) == 0x80)
                take--;
            if (!take)
                take = 1;
            if (row >= scroll) {
                char *line = strndup(ptr + pos, take);
                Rdraw_text(p, ui->font, ui->font_size, x, y + (row - scroll) * lh, line,
                           GUI_COLOR(ui, text), 1);
                free(line);
            }
            row++;
            pos += take;
            if (y + (row - scroll) * lh >= y + h)
                break;
        }
        ptr = end ? end + 1 : ptr + len;
    }
    Rdraw_pop_clip_rect(p);
}

static void gui_pane_paint(Gui *ui, RPainter *p, int side, float x, float y, float w, float h)
{
    GuiPane *pn = ui->pane + side;
    int active = ui->focus == side;
    Rdraw_rect(p, x, y, w, h, 8, GUI_COLOR(ui, panel), 1);
    Rdraw_rect_outline(
        p, x, y, w, h, 8, active ? 2 : 1,
        GUI_HEX_COLOR(active ? gui_theme_current(ui)->accent : gui_theme_current(ui)->border), 1);
    char *path = display_path(pn->path);
    char *search = display_path(pn->search);
    char *title =
        fmt("%s  /%s%s%s%s", side ? (ui->history ? "HISTORY" : "SOURCE") : "DESTINATION", path,
            (side && ui->filter) ? "  [UNSECURED]" : "", *search ? "  | name: " : "", search);
    free(search);
    free(path);
    gui_text_clip(ui, p, x + 12, y + 10, w - 24, 24, title, GUI_COLOR(ui, text), ui->font_size + 2);
    free(title);
    GuiTable layout = gui_table(ui, side);
    float *widths = layout.widths;
    float name = widths[0], status = widths[1], size = widths[2], time = widths[3];
    pn->horizontal = (int)fmaxf(0, fminf(fmaxf(0, layout.width - layout.body.w), pn->horizontal));
    float outer_x = x;
    x -= pn->horizontal;
    float top = y + 43, table = layout.body.y;
    const int columns[] = {SORT_NAME, SORT_STATUS, SORT_SIZE, pn->created ? SORT_BTIME : SORT_MTIME,
                           SORT_MODE};
    float xx = x + 8;
    Rdraw_push_clip_rect(p, layout.body.x, top - 4, layout.body.w, 28);
    for (int j = 0; j < 5; j++) {
        int sorted = (pn->sort & SORT_COLUMN_MASK) == columns[j];
        char label[64];
        gui_column_label(ui, side, j, label);
        gui_text_clip(
            ui, p, xx, top, widths[j] - 5, 24, label,
            GUI_HEX_COLOR(sorted ? gui_theme_current(ui)->accent : gui_theme_current(ui)->muted),
            ui->font_size - 1);
        xx += widths[j];
        int hot = ui->column_hover == side * GUI_FILE_COLUMNS + j + 1 ||
                  (ui->column_drag && ui->column_side == side && ui->column_index == j);
        Rdraw_rect(
            p, xx - 1, top, 1, 19, 0,
            GUI_HEX_COLOR(hot ? gui_theme_current(ui)->accent : gui_theme_current(ui)->border), 1);
    }
    Rdraw_pop_clip_rect(p);
    int count = gui_row_count(pn), visible = gui_table_visible(ui, side);
    if (pn->n) {
        int last = count > visible ? count - visible : 0;
        if (pn->offset > last)
            pn->offset = last;
        if (pn->offset < 0)
            pn->offset = 0;
    }
    Rdraw_push_clip_rect(p, layout.body.x, layout.body.y, layout.body.w, layout.body.h);
    for (int r = pn->offset; r < count && r < pn->offset + visible; r++) {
        int i = r - (*pn->path ? 1 : 0);
        float ry = table + (r - pn->offset) * ui->row_height;
        int sel = r == pn->selected;
        if (sel)
            Rdraw_rect(p, layout.body.x, ry, layout.body.w, ui->row_height - 1, 3,
                       GUI_HEX_COLOR(active ? gui_theme_current(ui)->selection
                                            : gui_theme_current(ui)->inactive),
                       1);
        else if (r % 2)
            Rdraw_rect(p, layout.body.x, ry, layout.body.w, ui->row_height - 1, 0,
                       GUI_COLOR(ui, alternate), 1);
        if (i < 0) {
            gui_text_clip(ui, p, x + 13, ry + 3, name - 9, ui->row_height, "[..]",
                          GUI_COLOR(ui, directory), ui->font_size);
            continue;
        }
        Entry *e = pn->rows + i;
        int dir = !strcmp(e->kind, "directory"),
            secured = !strcmp(e->state, "verified") || (!*e->hash && e->accepted);
        unsigned color = gui_theme_current(ui)->text;
        if (e->discarded)
            color = gui_theme_current(ui)->dim;
        else if (!strncmp(e->error, "UNSCANNED:", 10))
            color = gui_theme_current(ui)->warning;
        else if (*e->error || !strcmp(e->state, "corrupt") ||
                 (!strcmp(e->state, "missing") && *e->dest))
            color = gui_theme_current(ui)->error;
        else if (dir)
            color = gui_theme_current(ui)->directory;
        else if (secured)
            color = gui_theme_current(ui)->success;
        else if (e->copies > 1)
            color = gui_theme_current(ui)->duplicate;
        if (sel)
            color = gui_theme_current(ui)->text;
        GuiFileCells cells;
        gui_file_cells(ui, side, i, &cells);
        gui_text_clip(ui, p, x + 10, ry + 3, name - 4, ui->row_height, cells.name,
                      GUI_HEX_COLOR(color), ui->font_size);
        free(cells.name);
        gui_text_clip(ui, p, x + 8 + name, ry + 3, status - 4, ui->row_height, cells.status,
                      GUI_HEX_COLOR(color), ui->font_size - 1);
        gui_text_clip(ui, p, x + 8 + name + status, ry + 3, size - 4, ui->row_height, cells.size,
                      GUI_COLOR(ui, muted), ui->font_size - 1);
        gui_text_clip(ui, p, x + 8 + name + status + size, ry + 3, time - 4, ui->row_height,
                      cells.date, GUI_COLOR(ui, muted), ui->font_size - 2);
        gui_text_clip(ui, p, x + 8 + name + status + size + time, ry + 3, widths[4] - 12,
                      ui->row_height, cells.mode, GUI_COLOR(ui, muted), ui->font_size - 2);
    }
    Rdraw_pop_clip_rect(p);
    x = outer_x;
    double position =
        (double)pn->page * VIEW_PAGE_SIZE +
        (pn->scroll_pending ? pn->scroll_row : fmax(0, pn->offset - (*pn->path ? 1 : 0)));
    gui_scrollbar_paint(ui, p, side * 2,
                        (GuiRect){layout.body.x + layout.body.w + 3, table, 10, layout.body.h},
                        pn->total, visible, position, 0);
    gui_scrollbar_paint(ui, p, side * 2 + 1,
                        (GuiRect){layout.body.x, y + h - 36, layout.body.w, 10}, layout.width,
                        layout.body.w, pn->horizontal, 1);
    char footer[256];
    snprintf(footer, sizeof(footer), "Page %d/%lld | %d selected | Ctrl+PgUp/PgDn%s", pn->page + 1,
             (long long)(pn->total ? (pn->total - 1) / VIEW_PAGE_SIZE + 1 : 1), pn->mark_count,
             (ui->view_dirty & (side ? VIEW_RIGHT : VIEW_LEFT)) ? " | loading..." : "");
    gui_text_clip(ui, p, x + 12, y + h - 22, w - 24, 20, footer, GUI_COLOR(ui, muted),
                  ui->font_size - 2);
}

void gui_render(Gui *ui, RPainter *p, float w, float h)
{
    gui_scroll_begin(ui);
    ui->width = w;
    ui->height = h;
    Rdraw_rect(p, 0, 0, w, h, 0, GUI_COLOR(ui, background), 1);
    gui_text_clip(ui, p, 18, 12, w - 200, 28, "CCurate", GUI_COLOR(ui, text), 23);
    Rdraw_rect(p, w - 166, 8, 152, 28, 5, GUI_COLOR(ui, raised), 1);
    char scaling[60];
    snprintf(scaling, sizeof(scaling), "UI %.0f%%  Ctrl+,", ui->scale * 100);
    gui_text_clip(ui, p, w - 158, 14, 138, 22, scaling, GUI_COLOR(ui, text), 13);
    char a[40], b[40];
    human_bytes(atomic_load(&ui->c->storage.free_bytes), a);
    human_bytes(ui->summary.needed_bytes, b);
    char unique[40], saved[40];
    human_bytes(ui->summary.unique_bytes, unique);
    human_bytes(ui->summary.duplicate_bytes, saved);
    char *status =
        ui->summary.valid
            ? fmt("Free: %s | Unique%s: %s | Saved: %s | To secure: %s | OK: %llu | Rejected: %llu "
                  "| Ctrl+B",
                  a, ui->summary.unscanned_files || !ui->summary.traversal_complete ? "*" : "",
                  unique, saved, b, (unsigned long long)ui->summary.secured,
                  (unsigned long long)ui->summary.rejected)
            : fmt("Free: %s | Calculating space estimate... | Statistics: Ctrl+B", a);
    gui_text_clip(ui, p, 18, 42, w - 36, 21, status, GUI_COLOR(ui, accent), 13);
    free(status);
    char database[40], wal[40], index[40], db_total[40];
    uint64_t db_bytes = atomic_load(&ui->c->storage.database),
             wal_bytes = atomic_load(&ui->c->storage.wal),
             shm_bytes = atomic_load(&ui->c->storage.shm);
    human_bytes(db_bytes, database);
    human_bytes(wal_bytes, wal);
    human_bytes(atomic_load(&ui->c->storage.jsonl), index);
    human_bytes(db_bytes + wal_bytes + shm_bytes, db_total);
    char storage[256];
    snprintf(storage, sizeof(storage), "Database: %s | JSONL: %s | SQLite: %s | WAL: %s (+ SHM)",
             db_total, index, database, wal);
    gui_text_clip(ui, p, 18, 64, w - 36, 21, storage, GUI_COLOR(ui, muted), 13);
    char prog[1024];
    pthread_mutex_lock(&ui->c->progress_mutex);
    snprintf(prog, sizeof(prog), "%s", ui->c->progress);
    pthread_mutex_unlock(&ui->c->progress_mutex);
    char elapsed[40], eta[40], speed[40], done[40], planned[40], line[1024];
    duration_text(ui->scan.elapsed, elapsed);
    duration_text(ui->scan.eta, eta);
    human_bytes(ui->scan.bytes_per_second, speed);
    human_bytes(ui->scan.read_bytes, done);
    human_bytes(ui->scan.planned_bytes, planned);
    int scanning = ui->job && !strcmp(ui->job, "scan");
    int refreshing = atomic_load(&ui->busy) && ui->job && !strcmp(ui->job, "refresh");
    if (refreshing)
        snprintf(
            line, sizeof(line),
            "FOLDER CHECK: %llu entries | changed files %llu/%llu | SHA-512 read %s | errors %llu",
            (unsigned long long)ui->scan.found, (unsigned long long)ui->scan.hashed_files,
            (unsigned long long)ui->scan.hash_files, done, (unsigned long long)ui->scan.errors);
    else if (scanning && ui->scan.phase == SCAN_ENUMERATE)
        snprintf(line, sizeof(line),
                 "ENUMERATING: %llu entries | SHA-512: %s | elapsed %s | ETA: counting work",
                 (unsigned long long)ui->scan.found, planned, elapsed);
    else if (scanning && (ui->scan.phase == SCAN_ENUMERATE || ui->scan.phase == SCAN_HASH))
        snprintf(line, sizeof(line),
                 "SHA-512 %.1f%% | ETA ~%s | %s/s | %llu/%llu entries | %s/%s | errors %llu",
                 ui->scan.fraction * 100, eta, speed, (unsigned long long)ui->scan.done,
                 (unsigned long long)ui->scan.found, done, planned,
                 (unsigned long long)ui->scan.errors);
    else if (scanning && ui->scan.phase == SCAN_EXPORT)
        snprintf(line, sizeof(line),
                 "FINALIZING: writing archive index | %llu entries | elapsed %s",
                 (unsigned long long)ui->scan.done, elapsed);
    else
        snprintf(line, sizeof(line), "%s", ui->message ? ui->message : "Ready");
    if (*ui->log_error)
        snprintf(line, sizeof(line), "LOG ERROR: %s", ui->log_error);
    gui_text_clip(
        ui, p, 18, 87, w - 36, 22, line,
        GUI_HEX_COLOR(*ui->log_error ? gui_theme_current(ui)->error : gui_theme_current(ui)->text),
        13);
    float bar_y = gui_pane_top(ui) - 12;
    Rdraw_rect(p, 18, bar_y, w - 36, 4, 2, GUI_COLOR(ui, border), 1);
    if (scanning && ui->scan.fraction >= 0)
        Rdraw_rect(p, 18, bar_y, (w - 36) * ui->scan.fraction, 4, 2, GUI_COLOR(ui, accent), 1);
    else if (atomic_load(&ui->busy)) {
        float t = fmod(Rget_time(), 2) / 2;
        Rdraw_rect(p, 18 + (w - 76) * t, bar_y, 40, 4, 2, GUI_COLOR(ui, accent), 1);
    }
    if (ui->height >= 420) {
        char *safe = display_path(prog);
        gui_text_clip(ui, p, 18, 110, w - 36, 20, safe, GUI_COLOR(ui, muted), 12);
        free(safe);
    }
    GuiGeometry g = gui_geometry(ui);
    for (int i = 0; i < 2; i++) {
        GuiRect r = g.pane[i];
        gui_pane_paint(ui, p, i, r.x, r.y, r.w, r.h);
    }
    for (int i = 2; i < 4; i++) {
        GuiRect r = g.pane[i];
        Rdraw_rect(p, r.x, r.y, r.w, r.h, 8, GUI_COLOR(ui, panel), 1);
        Rdraw_rect_outline(p, r.x, r.y, r.w, r.h, 8, ui->focus == i ? 2 : 1,
                           GUI_HEX_COLOR(ui->focus == i ? gui_theme_current(ui)->accent
                                                        : gui_theme_current(ui)->border),
                           1);
    }
    GuiRect pv = g.pane[2], dt = g.pane[3];
    gui_text_clip(ui, p, pv.x + 12, pv.y + 9, pv.w - 24, 24,
                  "PREVIEW | [ ] page / frame | Ctrl+X hex", GUI_COLOR(ui, muted),
                  ui->font_size - 1);
    gui_text_clip(ui, p, dt.x + 12, dt.y + 9, dt.w - 24, 24,
                  ui->help ? "INFORMATION | arrows / PgUp / PgDn"
                           : "DETAILS / OCCURRENCES | Ctrl+D",
                  GUI_COLOR(ui, muted), ui->font_size - 1);
    GuiRect preview_body = {pv.x + 12, pv.y + 40, fmaxf(1, pv.w - 32), fmaxf(1, pv.h - 58)};
    GuiRect details_body = {dt.x + 12, dt.y + 40, fmaxf(1, dt.w - 32), fmaxf(1, dt.h - 58)};
    if (ui->texture != R_INVALID_ID && ui->preview.width && ui->preview.height) {
        gui_text_panel_paint(ui, p, 2, preview_body, "");
        float scale =
            fminf(preview_body.w / ui->preview.width, preview_body.h / ui->preview.height);
        float iw = ui->preview.width * scale, ih = ui->preview.height * scale;
        Rdraw_image(p, ui->texture, preview_body.x + (preview_body.w - iw) / 2,
                    preview_body.y + (preview_body.h - ih) / 2, iw, ih, 1, 1, 1, 1);
    } else
        gui_text_panel_paint(ui, p, 2, preview_body, ui->preview.text);
    Buf details = {0};
    badd(&details, "%s", ui->details ? ui->details : "");
    if (ui->texture != R_INVALID_ID && ui->preview.text)
        badd(&details, "\n--- PREVIEW ---\n%s", ui->preview.text);
    gui_text_panel_paint(ui, p, 3, details_body, details.s);
    bfree(&details);
    for (int i = 0; i < 4; i++) {
        GuiRect r = g.split[i];
        if (r.h <= 0)
            continue;
        int hot = ui->drag == i + 1 || ui->splitter_hover == i + 1;
        if (i < 2) {
            r.x += 4;
            r.w = 4;
            r.y += r.h * .35f;
            r.h *= .3f;
        } else {
            r.y += 4;
            r.h = 4;
            r.x += r.w * .4f;
            r.w *= .2f;
        }
        Rdraw_rect(
            p, r.x, r.y, r.w, r.h, 2,
            GUI_HEX_COLOR(hot ? gui_theme_current(ui)->accent : gui_theme_current(ui)->border), 1);
    }
    if (ui->log_visible)
        gui_log_paint(ui, p, g);
    gui_menu_paint(ui, p);
    gui_dropdown_paint(ui, p);
    gui_font_picker_paint(ui, p);
}
