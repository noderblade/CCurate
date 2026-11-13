#include "gui_render.h"
#include "gui_scroll.h"
#include "gui_state.h"
#include "gui_theme.h"
#include "gui_usage_state.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static const char *column_names[] = {"Name",          "Logical size", "Share",     "Unique here",
                                     "Repeated here", "Files",        "Unverified"};
static const char *commands[] = {
    "usage.up",     "usage.root",     "usage.rings",   "usage.treemap",      "usage.depth-",
    "usage.depth+", "usage.scope",    "usage.columns", "usage.rebuild",      "usage.cancel",
    "usage.reveal", "usage.previous", "usage.next",    "usage.occurrences-", "usage.occurrences+"};
static const char *labels[] = {"Up",
                               "Root",
                               "Rings",
                               "Treemap",
                               "Level -",
                               "Level +",
                               "All entries",
                               "Columns",
                               "Rebuild",
                               "Cancel",
                               "Reveal in Curation",
                               "Previous page",
                               "Next page",
                               "Previous occurrences",
                               "Next occurrences"};
static const char *usage_help =
    "DISK USAGE — LOGICAL CONTENT SIZES\n"
    "Ctrl+Tab              switch Curation / Disk Usage\n"
    "Arrows, PgUp/PgDn      select entries; Home/End: first/last row\n"
    "Enter / double-click  open directory; reveal files in Curation\n"
    "Space / Right         expand or collapse a directory\n"
    "Backspace / Alt+Left   parent directory; Ctrl+Home: source root\n"
    "Ctrl+PgUp/PgDn         previous / next directory-list page\n"
    "[ / ]                 decrease / increase chart depth (1–5)\n"
    "R / T                 Rings / Treemap\n"
    "Ctrl+F                All entries / Keep candidates\n"
    "Ctrl+S                choose sort column; Shift+Ctrl+S: reverse\n"
    "Ctrl+Alt+C / W        visible columns / fit column widths\n"
    "Ctrl+Alt+L            cycle tree/chart proportions\n"
    "F3 / Ctrl+D           reveal in Curation / focus analysis details\n"
    "Tab                   switch tree / details focus\n"
    "Ctrl+Alt+PgUp/PgDn    pages of matching source occurrences\n"
    "Esc                   close help or cancel index preparation\n"
    "F2                    menu; View contains every analysis action\n\n"
    "Click a header to sort; drag its right edge to resize.\n"
    "Double-click a header divider to fit the displayed rows.\n"
    "Drag the panel separators or their scrollbars. Shift+wheel pans.\n"
    "Click a chart folder to drill down; click the ring center to go up.\n"
    "Hover only shows information; it never selects an entry.\n\n"
    "Logical size: sum of file lengths, including observed unverified sizes.\n"
    "Unique here: one copy of each verified nonempty hash in this subtree.\n"
    "Repeated here: additional verified occurrences within this subtree.\n"
    "Unique and repeated totals are NOT additive across sibling folders.\n"
    "Unverified: changed, unreadable or unhashed files; excluded from deduplication.\n"
    "An asterisk means the deduplication totals cover only the verified subset.\n"
    "Empty files are independent entries. Symlinks/special entries count as 0 B\n"
    "of file content; their metadata is preserved and links are never followed.\n"
    "Other entries: remaining chart bytes; all entries remain in the tree.\n"
    "Keep candidates excludes rejected files, retaining directory containers.\n"
    "The view reflects the saved source catalog, not an automatic live scan.\n"
    "Rebuild reads SQLite only. Copy/review actions are available in Curation.\n";

const char *gui_usage_column_name(int column)
{
    return column >= 0 && column < USAGE_COLUMNS ? column_names[column] : "";
}
const char *gui_usage_button_command(int button)
{
    return button >= 0 && button < USAGE_BUTTONS ? commands[button] : "";
}
GuiUsageGeometry gui_usage_geometry(Gui *ui)
{
    GuiUsage *u = ui->usage;
    GuiUsageGeometry g = {0};
    float top = gui_pane_top(ui), x = 12, y = top + 72;
    g.breadcrumb = (GuiRect){12, top, ui->width - 24, 28};
    /* Keep the root and the nearest ancestors reachable even for long paths. */
    g.crumbs[0] = (GuiRect){12, top, 88, 24};
    g.crumb_count = 1;
    size_t ends[USAGE_CRUMBS - 1], count = 0, length = strlen(u->request.path);
    for (size_t end = length; end && count < USAGE_CRUMBS - 1;) {
        ends[count++] = end;
        while (end && u->request.path[end - 1] != '/')
            end--;
        if (end)
            end--;
    }
    float used = 104;
    int first = (int)count - 1;
    for (int i = 0; i < (int)count; i++) {
        size_t begin = ends[i];
        while (begin && u->request.path[begin - 1] != '/')
            begin--;
        char *raw = strndup(u->request.path + begin, ends[i] - begin);
        char *safe = display_path(raw);
        float width = 0;
        Rfont_measure_text(ui->font, 13, safe, &width, NULL);
        free(raw);
        free(safe);
        width = fminf(220, fmaxf(50, width + 24));
        if (used + width > ui->width - 20 && i) {
            first = i - 1;
            break;
        }
        used += width + 4;
    }
    float bx = 104;
    for (int i = first; i >= 0; i--) {
        size_t begin = ends[i];
        while (begin && u->request.path[begin - 1] != '/')
            begin--;
        char *raw = strndup(u->request.path + begin, ends[i] - begin);
        char *safe = display_path(raw);
        float width = 0;
        Rfont_measure_text(ui->font, 13, safe, &width, NULL);
        free(raw);
        free(safe);
        width = fminf(220, fmaxf(50, width + 24));
        int n = g.crumb_count++;
        g.crumb_end[n] = ends[i];
        g.crumbs[n] = (GuiRect){bx, top, width, 24};
        bx += width + 4;
    }
    for (int i = 0; i < 11; i++) {
        float width = (i == 6 ? 16 : (int)strlen(labels[i])) * 8 + 18;
        if (x + width > ui->width - 12 && x > 12) {
            x = 12;
            y += 33;
        }
        g.buttons[i] = (GuiRect){x, y, width, 28};
        x += width + 5;
    }
    top = y + 38;
    float bottom = ui->height - 49;
    if (ui->log_visible)
        bottom = gui_geometry(ui).pane[4].y - 14;
    float available = fmaxf(30, bottom - top), dh = fmaxf(70, available * u->details_split);
    dh = fminf(dh, available * .55f);
    float upper = fmaxf(1, available - dh - 12);
    g.details = (GuiRect){12, top + upper + 12, ui->width - 24, dh};
    g.detail_split = (GuiRect){12, top + upper, ui->width - 24, 12};
    if (ui->width >= 760) {
        float left = (ui->width - 36) * u->split;
        g.tree = (GuiRect){12, top, left, upper};
        g.chart = (GuiRect){24 + left, top, ui->width - 36 - left, upper};
        g.tree_split = (GuiRect){12 + left, top, 12, upper};
    } else {
        float th = (upper - 12) * u->split;
        g.tree = (GuiRect){12, top, ui->width - 24, fmaxf(1, th)};
        g.chart = (GuiRect){12, top + th + 12, ui->width - 24, fmaxf(1, upper - th - 12)};
        g.tree_split = (GuiRect){12, top + th, ui->width - 24, 12};
    }
    g.body =
        (GuiRect){g.tree.x + 8, g.tree.y + 64, fmaxf(1, g.tree.w - 28), fmaxf(1, g.tree.h - 116)};
    g.visible = (int)fmaxf(1, g.body.h / ui->row_height);
    x = g.body.x - u->horizontal;
    for (int i = 0; i < USAGE_COLUMNS; i++)
        if (u->columns & (1u << i)) {
            g.columns[i] = (GuiRect){x, g.tree.y + 35, u->widths[i], 25};
            x += u->widths[i];
            g.table_width += u->widths[i];
        }
    g.buttons[11] = (GuiRect){g.tree.x + 8, g.tree.y + g.tree.h - 28, 112, 23};
    g.buttons[12] = (GuiRect){g.tree.x + 126, g.tree.y + g.tree.h - 28, 90, 23};
    g.buttons[13] = (GuiRect){fmaxf(g.details.x + 150, ui->width - 390), g.details.y + 6, 176, 24};
    g.buttons[14] = (GuiRect){ui->width - 204, g.details.y + 6, 180, 24};
    return g;
}
void gui_usage_cell(Gui *ui, const UsageRow *row, int col, char *out, size_t size)
{
    char bytes[40];
    switch (col) {
    case USAGE_NAME: {
        char *name = display_path(row->name);
        snprintf(out, size, "%s%s", name, !strcmp(row->kind, "directory") && !row->more ? "/" : "");
        free(name);
        break;
    }
    case USAGE_SHARE:
        if (ui->usage->result && ui->usage->result->totals.bytes > 0)
            snprintf(out, size, "%.1f%%",
                     100.0 * row->totals.bytes / ui->usage->result->totals.bytes);
        else
            snprintf(out, size, "—");
        break;
    case USAGE_FILES:
        snprintf(out, size, "%lld", (long long)row->totals.files);
        break;
    default: {
        int64_t value = col == USAGE_BYTES      ? row->totals.bytes
                        : col == USAGE_UNIQUE   ? row->totals.hashed - row->totals.repeated
                        : col == USAGE_REPEATED ? row->totals.repeated
                                                : row->totals.unknown_bytes;
        human_bytes(value < 0 ? 0 : value, bytes);
        snprintf(out, size, "%s%s", bytes,
                 ((col == USAGE_UNIQUE || col == USAGE_REPEATED) && row->totals.unknown_files)
                     ? "*"
                     : "");
        break;
    }
    }
}
void gui_usage_fit(Gui *ui, int column)
{
    GuiUsage *u = ui->usage;
    GuiUsageGeometry g = gui_usage_geometry(ui);
    for (int col = 0; col < USAGE_COLUMNS; col++)
        if (column < 0 || column == col) {
            float width = 0;
            Rfont_measure_text(ui->font, ui->font_size, column_names[col], &width, NULL);
            width += 22;
            for (int i = u->offset; u->result && i < u->result->count && i < u->offset + g.visible;
                 i++) {
                char text[1024];
                float measured = 0;
                gui_usage_cell(ui, u->result->rows + i, col, text, sizeof(text));
                Rfont_measure_text(ui->font, ui->font_size, text, &measured, NULL);
                if (col == USAGE_NAME)
                    measured += 40 + u->result->rows[i].depth * 18;
                width = fmaxf(width, measured + 12);
            }
            u->widths[col] = fminf(2000, fmaxf(55, width));
        }
    gui_usage_save(ui);
}
static void button(Gui *ui, RPainter *p, GuiRect r, const char *label, int selected)
{
    if (r.w <= 0 || r.h <= 0)
        return;
    Rdraw_rect(
        p, r.x, r.y, r.w, r.h, 4,
        GUI_HEX_COLOR(selected ? gui_theme_current(ui)->selection : gui_theme_current(ui)->raised),
        1);
    gui_text_clip(ui, p, r.x + 7, r.y + 4, r.w - 14, r.h - 4, label, GUI_COLOR(ui, text), 12);
}
static void panel(Gui *ui, RPainter *p, GuiRect r, int focus)
{
    Rdraw_rect(p, r.x, r.y, r.w, r.h, 6, GUI_COLOR(ui, panel), 1);
    Rdraw_rect_outline(
        p, r.x, r.y, r.w, r.h, 6, focus ? 2 : 1,
        GUI_HEX_COLOR(focus ? gui_theme_current(ui)->accent : gui_theme_current(ui)->border), 1);
}
static void details_paint(Gui *ui, RPainter *p, GuiRect r)
{
    GuiUsage *u = ui->usage;
    const char *text = u->help     ? usage_help
                       : u->result ? u->result->details
                                   : "Select an entry after the usage index is ready.";
    float line_height = ui->font_size + 5;
    GuiRect body = {r.x + 10, r.y + 38, fmaxf(1, r.w - 30), fmaxf(1, r.h - 56)};
    int row = 0;
    float width = 0;
    Rdraw_push_clip_rect(p, body.x, body.y, body.w, body.h);
    const char *at = text;
    while (at && *at) {
        const char *end = strchr(at, '\n');
        char *line = end ? strndup(at, end - at) : xstrdup(at);
        float measured = 0;
        Rfont_measure_text(ui->font, ui->font_size, line, &measured, NULL);
        width = fmaxf(width, measured);
        if (row >= u->details_scroll && (row - u->details_scroll) * line_height < body.h)
            Rdraw_text(p, ui->font, ui->font_size, body.x - u->details_horizontal,
                       body.y + (row - u->details_scroll) * line_height, line, GUI_COLOR(ui, text),
                       1);
        free(line);
        row++;
        if (!end)
            break;
        at = end + 1;
    }
    Rdraw_pop_clip_rect(p);
    u->details_lines = row;
    u->details_width = width;
    u->details_scroll = (int)fmaxf(0, fminf(u->details_scroll, row - floorf(body.h / line_height)));
    u->details_horizontal = (int)fmaxf(0, fminf(u->details_horizontal, width - body.w));
    gui_scrollbar_paint(ui, p, 16, (GuiRect){body.x + body.w + 4, body.y, 10, body.h}, row,
                        fmaxf(1, body.h / line_height), u->details_scroll, 0);
    gui_scrollbar_paint(ui, p, 17, (GuiRect){body.x, r.y + r.h - 13, body.w, 9}, width, body.w,
                        u->details_horizontal, 1);
}
void gui_usage_paint(Gui *ui, RPainter *p)
{
    GuiUsage *u = ui->usage;
    GuiUsageGeometry g = gui_usage_geometry(ui);
    for (int i = 0; i < g.crumb_count; i++) {
        size_t end = g.crumb_end[i], begin = end;
        while (begin && u->request.path[begin - 1] != '/')
            begin--;
        char *raw = i ? strndup(u->request.path + begin, end - begin) : xstrdup("SOURCE /");
        char *label = display_path(raw);
        button(ui, p, g.crumbs[i], label, i == g.crumb_count - 1);
        free(raw);
        free(label);
    }
    char logical[40], unique[40], repeated[40], unknown[40], metrics[512];
    UsageTotals t = u->result ? u->result->totals : (UsageTotals){0};
    human_bytes(t.bytes, logical);
    human_bytes(t.hashed - t.repeated, unique);
    human_bytes(t.repeated, repeated);
    human_bytes(t.unknown_bytes, unknown);
    snprintf(metrics, sizeof(metrics), "Logical: %s | Unique here%s: %s | Repeated here%s: %s",
             logical, t.unknown_files ? "*" : "", unique, t.unknown_files ? "*" : "", repeated);
    gui_text_clip(ui, p, 17, g.breadcrumb.y + 25, ui->width - 34, 22, metrics,
                  GUI_COLOR(ui, accent), 13);
    snprintf(metrics, sizeof(metrics), "Unverified: %s / %lld files | Chart depth: %d | %s%s",
             unknown, (long long)t.unknown_files, u->request.depth,
             u->request.keep ? "Keep candidates" : "All present entries",
             u->result && !u->result->complete ? " | Incomplete source traversal" : "");
    gui_text_clip(ui, p, 17, g.breadcrumb.y + 47, ui->width - 34, 22, metrics, GUI_COLOR(ui, muted),
                  12);
    for (int i = 0; i < 11; i++)
        button(ui, p, g.buttons[i],
               i == 6 ? (u->request.keep ? "Keep candidates" : "All entries") : labels[i],
               (i == 2 && !u->treemap) || (i == 3 && u->treemap));
    panel(ui, p, g.tree, u->focus == 0);
    panel(ui, p, g.chart, 0);
    panel(ui, p, g.details, u->focus == 1);
    gui_text_clip(ui, p, g.tree.x + 10, g.tree.y + 8, g.tree.w - 20, 23, "SOURCE TREE",
                  GUI_COLOR(ui, muted), ui->font_size);
    gui_text_clip(ui, p, g.chart.x + 10, g.chart.y + 8, g.chart.w - 20, 23,
                  "LOGICAL SIZE · CURRENT DIRECTORY = 100%", GUI_COLOR(ui, muted), 12);
    u->horizontal = (int)fmaxf(0, fminf(u->horizontal, g.table_width - g.body.w));
    Rdraw_push_clip_rect(p, g.body.x, g.tree.y + 33, g.body.w, 29);
    for (int col = 0; col < USAGE_COLUMNS; col++)
        if (u->columns & (1u << col)) {
            GuiRect r = g.columns[col];
            char name[64];
            snprintf(name, sizeof(name), "%s%s", column_names[col],
                     u->request.sort == col ? (u->request.descending ? " v" : " ^") : "");
            gui_text_clip(ui, p, r.x + 4, r.y, r.w - 9, 25, name,
                          GUI_HEX_COLOR(u->request.sort == col ? gui_theme_current(ui)->accent
                                                               : gui_theme_current(ui)->muted),
                          12);
            Rdraw_rect(p, r.x + r.w - 1, r.y, 1, 21, 0,
                       GUI_HEX_COLOR(u->column_hover == col + 1 ? gui_theme_current(ui)->accent
                                                                : gui_theme_current(ui)->border),
                       1);
        }
    Rdraw_pop_clip_rect(p);
    int same = u->result && !strcmp(u->result->path, u->request.path);
    int count = same ? u->result->count : 0;
    u->offset = (int)fmaxf(0, fminf(u->offset, count - g.visible));
    Rdraw_push_clip_rect(p, g.body.x, g.body.y, g.body.w, g.body.h);
    for (int i = u->offset; i < count && i < u->offset + g.visible; i++) {
        UsageRow *row = u->result->rows + i;
        float y = g.body.y + (i - u->offset) * ui->row_height;
        if (i == u->selected)
            Rdraw_rect(p, g.body.x, y, g.body.w, ui->row_height - 1, 2, GUI_COLOR(ui, selection),
                       1);
        else if (i % 2)
            Rdraw_rect(p, g.body.x, y, g.body.w, ui->row_height - 1, 0, GUI_COLOR(ui, alternate),
                       1);
        for (int col = 0; col < USAGE_COLUMNS; col++)
            if (u->columns & (1u << col)) {
                GuiRect c = g.columns[col];
                char cell[2048];
                gui_usage_cell(ui, row, col, cell, sizeof(cell));
                float indent = col == 0 ? row->depth * 18 + 37 : 4;
                if (col == 0 && u->result) {
                    for (int n = 0; n < u->result->node_count; n++)
                        if (!strcmp(row->path, u->result->nodes[n].row.path)) {
                            Rdraw_rect(p, c.x + indent - 13, y + 8, 7, 7, 1,
                                       GUI_HEX_COLOR(gui_usage_node_color(ui, n)), 1);
                            break;
                        }
                }
                if (col == 0 && !strcmp(row->kind, "directory") && !row->more) {
                    int expanded = 0;
                    for (int j = 0; j < u->request.expanded_count; j++)
                        if (!strcmp(row->path, u->request.expanded[j]))
                            expanded = 1;
                    gui_text_clip(ui, p, c.x + row->depth * 18 + 3, y + 3, 20, ui->row_height,
                                  expanded ? "−" : "+", GUI_COLOR(ui, directory), ui->font_size);
                }
                unsigned color =
                    col == 0 ? (!strcmp(row->kind, "directory") ? gui_theme_current(ui)->directory
                                                                : gui_theme_current(ui)->text)
                             : gui_theme_current(ui)->muted;
                if (col == USAGE_UNKNOWN && row->totals.unknown_files)
                    color = gui_theme_current(ui)->warning;
                if (col == USAGE_REPEATED && row->totals.repeated)
                    color = gui_theme_current(ui)->duplicate;
                if (row->discarded && !strcmp(row->kind, "file"))
                    color = gui_theme_current(ui)->dim;
                gui_text_clip(ui, p, c.x + indent, y + 3, c.w - indent - 5, ui->row_height, cell,
                              GUI_HEX_COLOR(color), ui->font_size - 1);
            }
    }
    if (!count)
        gui_text_clip(ui, p, g.body.x + 8, g.body.y + 8, g.body.w - 16, 40,
                      u->error ? u->error
                      : u->status.phase == USAGE_ERROR || u->status.phase == USAGE_CANCELLED
                          ? u->status.message
                      : u->loading ? "Loading catalog analysis..."
                                   : "No entries in this directory.",
                      GUI_COLOR(ui, muted), ui->font_size);
    Rdraw_pop_clip_rect(p);
    gui_scrollbar_paint(ui, p, 14, (GuiRect){g.body.x + g.body.w + 4, g.body.y, 10, g.body.h},
                        count, g.visible, u->offset, 0);
    gui_scrollbar_paint(ui, p, 15, (GuiRect){g.body.x, g.tree.y + g.tree.h - 45, g.body.w, 10},
                        g.table_width, g.body.w, u->horizontal, 1);
    for (int i = 11; i < 13; i++)
        button(ui, p, g.buttons[i], labels[i], 0);
    char footer[128];
    snprintf(footer, sizeof(footer), "%d/%lld%s", u->request.page + 1,
             (long long)(u->result && u->result->children
                             ? (u->result->children - 1) / USAGE_PAGE + 1
                             : 1),
             u->loading ? " · loading" : "");
    gui_text_clip(ui, p, g.tree.x + 226, g.tree.y + g.tree.h - 23, g.tree.w - 236, 20, footer,
                  GUI_COLOR(ui, muted), 12);
    if (same)
        gui_usage_chart_paint(
            ui, p,
            (GuiRect){g.chart.x + 8, g.chart.y + 35, g.chart.w - 16, fmaxf(1, g.chart.h - 88)});
    if (same && u->hover >= 0 && u->hover < u->result->node_count) {
        UsageNode *node = u->result->nodes + u->hover;
        char bytes[40];
        human_bytes(node->row.totals.bytes, bytes);
        char *safe = display_path(node->row.name);
        char *label = fmt("%s | %s | %.1f%%", safe, bytes,
                          t.bytes ? 100.0 * node->row.totals.bytes / t.bytes : 0);
        free(safe);
        gui_text_clip(ui, p, g.chart.x + 10, g.chart.y + g.chart.h - 44, g.chart.w - 20, 40, label,
                      GUI_COLOR(ui, text), 12);
        free(label);
    } else
        gui_text_clip(ui, p, g.chart.x + 10, g.chart.y + g.chart.h - 44, g.chart.w - 20, 40,
                      "Click: drill down · Hover: details only", GUI_COLOR(ui, muted), 12);
    gui_text_clip(ui, p, g.details.x + 10, g.details.y + 9, fmaxf(1, g.details.w - 410), 22,
                  u->help ? "DISK USAGE HELP" : "DETAILS / OCCURRENCES", GUI_COLOR(ui, muted), 12);
    if (g.details.w > 650)
        for (int i = 13; i < 15; i++)
            button(ui, p, g.buttons[i], labels[i], 0);
    details_paint(ui, p, g.details);
    GuiRect splits[] = {g.tree_split, g.detail_split};
    for (int i = 0; i < 2; i++) {
        GuiRect r = splits[i];
        if (i == 0 && ui->width >= 760) {
            r.x += 4;
            r.w = 4;
            r.y += r.h * .35f;
            r.h *= .3f;
        } else {
            r.y += 4;
            r.h = 4;
            r.x += r.w * .35f;
            r.w *= .3f;
        }
        Rdraw_rect(p, r.x, r.y, r.w, r.h, 2, GUI_COLOR(ui, border), 1);
    }
    if (u->popup) {
        GuiRect r = {g.tree.x + 8, g.tree.y + 30, fminf(330, ui->width - 40),
                     USAGE_COLUMNS * 30 + 62};
        r.y = fmaxf(10, fminf(r.y, ui->height - r.h - 45));
        panel(ui, p, r, 1);
        gui_text_clip(ui, p, r.x + 10, r.y + 8, r.w - 20, 24,
                      u->popup == 1 ? "VISIBLE COLUMNS · Space toggles"
                                    : "SORT COLUMN · Enter selects",
                      GUI_COLOR(ui, text), 12);
        for (int i = 0; i < USAGE_COLUMNS; i++) {
            if (i == u->popup_row)
                Rdraw_rect(p, r.x + 4, r.y + 34 + i * 30, r.w - 8, 28, 3, GUI_COLOR(ui, selection),
                           1);
            char label[80];
            snprintf(label, sizeof(label), "%s %s",
                     u->popup == 1 ? (u->columns & (1u << i) ? "[x]" : "[ ]") : "",
                     column_names[i]);
            gui_text_clip(ui, p, r.x + 12, r.y + 39 + i * 30, r.w - 24, 25, label,
                          GUI_COLOR(ui, text), 13);
        }
        gui_text_clip(ui, p, r.x + 10, r.y + r.h - 22, r.w - 20, 20,
                      "Esc closes · Name stays visible", GUI_COLOR(ui, muted), 11);
    }
}
