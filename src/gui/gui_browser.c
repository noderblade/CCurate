#include "gui_browser.h"
#include "gui_controller.h"
#include "gui_jobs.h"
#include "gui_layout.h"
#include "gui_navigation.h"
#include "gui_state.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Paged browsing, selection, navigation and asynchronous view results. */

static int gui_mark_find(GuiPane *p, const char *path);
static int gui_is_marked(GuiPane *p, const char *path);
static void gui_apply_pane(Gui *ui, int side, ViewResult *r);
static char *gui_selection_signature(Gui *ui);

int gui_row_count(const GuiPane *p)
{
    return p->n + (*p->path ? 1 : 0);
}

Entry *gui_selected(GuiPane *p)
{
    int i = p->selected - (*p->path ? 1 : 0);
    return i >= 0 && i < p->n ? p->rows + i : NULL;
}

int gui_selected_index(GuiPane *p)
{
    return p->selected - (*p->path ? 1 : 0);
}

void gui_set_focus(Gui *ui, int focus)
{
    ui->focus = focus;
    if (focus < 2 && ui->inspection_side != focus) {
        ui->inspection_side = focus;
        gui_request_preview(ui, 1);
    }
}

void gui_request_view(Gui *ui, unsigned flags)
{
    if (!ui->view)
        return;
    ui->view_dirty |= flags;
    Entry *e = gui_selected(ui->pane + ui->inspection_side);
    ViewRequest r = {.flags = ui->view_dirty,
                     .refresh_summary = !!(flags & (VIEW_SUMMARY | VIEW_BALANCE)),
                     .historical = ui->history,
                     .unsecured = ui->filter,
                     .fast = gui_operation_busy(ui),
                     .target_refresh = ui->target_refresh,
                     .occurrence_page = ui->occurrence_page,
                     .detail_destination = ui->inspection_side == 0,
                     .detail_path = e ? e->path : "",
                     .detail_version = e ? e->version : 0};
    for (int i = 0; i < 2; i++) {
        r.path[i] = ui->pane[i].path;
        r.search[i] = ui->pane[i].search;
        r.anchor[i] = ui->pane[i].anchor;
        r.page[i] = ui->pane[i].page;
        r.sort[i] = ui->pane[i].sort;
    }
    ui->view_serial = view_submit(ui->view, &r);
}

static int gui_mark_find(GuiPane *p, const char *path)
{
    int low = 0, high = p->mark_count;
    while (low < high) {
        int mid = low + (high - low) / 2;
        if (strcmp(p->marks[mid], path) < 0)
            low = mid + 1;
        else
            high = mid;
    }
    return low;
}

static int gui_is_marked(GuiPane *p, const char *path)
{
    int i = gui_mark_find(p, path);
    return i < p->mark_count && !strcmp(p->marks[i], path);
}

void gui_toggle_mark(GuiPane *p, int row)
{
    if (row < 0 || row >= p->n)
        return;
    const char *path = p->rows[row].path;
    int i = gui_mark_find(p, path);
    if (i < p->mark_count && !strcmp(p->marks[i], path)) {
        free(p->marks[i]);
        memmove(p->marks + i, p->marks + i + 1, (p->mark_count - i - 1) * sizeof(char *));
        p->mark_count--;
    } else {
        p->marks = realloc(p->marks, (p->mark_count + 1) * sizeof(char *));
        memmove(p->marks + i + 1, p->marks + i, (p->mark_count - i) * sizeof(char *));
        p->marks[i] = xstrdup(path);
        p->mark_count++;
    }
    p->marked[row] = gui_is_marked(p, path);
}

void gui_clear_marks(GuiPane *p)
{
    for (int i = 0; i < p->mark_count; i++)
        free(p->marks[i]);
    free(p->marks);
    p->marks = NULL;
    p->mark_count = 0;
    if (p->marked)
        memset(p->marked, 0, p->n);
}

void gui_pane_reload(Gui *ui, int side)
{
    gui_request_view(ui, side ? VIEW_RIGHT : VIEW_LEFT);
}

static void gui_apply_pane(Gui *ui, int side, ViewResult *r)
{
    GuiPane *p = ui->pane + side;
    Entry *old = gui_selected(p);
    int restoring = p->anchor != NULL;
    char *keep = p->anchor ? p->anchor : old ? xstrdup(old->path) : NULL;
    p->anchor = NULL;
    entries_free(p->rows, p->n);
    free(p->marked);
    p->rows = r->rows[side];
    p->n = r->count[side];
    r->rows[side] = NULL;
    r->count[side] = 0;
    p->page = r->page[side];
    p->total = r->total[side];
    p->marked = xmalloc(p->n);
    p->name_chars = 0;
    for (int i = 0; i < p->n; i++) {
        p->marked[i] = gui_is_marked(p, p->rows[i].path);
        char *display = display_path(p->rows[i].name);
        int chars = 0;
        for (const unsigned char *ch = (const unsigned char *)display; *ch; ch++)
            chars += (*ch & 0xc0) != 0x80;
        if (chars > p->name_chars)
            p->name_chars = chars;
        free(display);
        if (keep && !strcmp(keep, p->rows[i].path))
            p->selected = i + (*p->path ? 1 : 0);
    }
    free(keep);
    if (p->page_end) {
        p->selected = gui_row_count(p) - 1;
        p->page_end = 0;
    }
    if (p->selected >= gui_row_count(p))
        p->selected = gui_row_count(p) - 1;
    if (p->selected < 0)
        p->selected = 0;
    if (p->scroll_pending) {
        p->selected = p->scroll_row + (*p->path ? 1 : 0);
        if (p->selected >= gui_row_count(p))
            p->selected = gui_row_count(p) > 0 ? gui_row_count(p) - 1 : 0;
        p->offset = p->scroll_row == 0 ? 0 : p->selected;
        p->scroll_pending = 0;
    }
    int last_offset = gui_row_count(p) - gui_table_visible(ui, side);
    if (p->offset > last_offset)
        p->offset = last_offset > 0 ? last_offset : 0;
    if (restoring) {
        gui_scroll_selection(ui, p);
        Entry *entry = gui_selected(p);
        activity_log(ui->c, LOG_DEBUG, "navigation.restore", entry ? entry->path : p->path, NULL,
                     "Pane %d: page %d, row %d, scroll %d", side, p->page, p->selected, p->offset);
    }
}

static char *gui_selection_signature(Gui *ui)
{
    Entry *e = gui_selected(ui->pane + ui->inspection_side);
    return e ? fmt("%s|%lld|%s|%s|%d|%d|%d|%d|%lld|%lld|%lld|%s", e->path, (long long)e->version,
                   e->dest, e->state, e->copies, e->reviewed, e->discarded, e->accepted,
                   (long long)e->size, (long long)e->mtime, (long long)e->mode, e->hash)
             : xstrdup("");
}

void gui_apply_view(Gui *ui)
{
    ViewResult *r = view_take(ui->view);
    if (!r)
        return;
    if (r->serial == ui->view_serial) {
        char *before = gui_selection_signature(ui);
        ui->view_dirty &= ~r->flags;
        if (r->flags & VIEW_LEFT) {
            gui_apply_pane(ui, 0, r);
            ui->target_refresh = 0;
        }
        if (r->flags & VIEW_RIGHT)
            gui_apply_pane(ui, 1, r);
        if (r->flags & VIEW_SUMMARY)
            ui->summary = r->summary;
        if (r->details && !ui->help) {
            free(ui->details);
            ui->details = r->details;
            r->details = NULL;
        }
        if (r->balance && ui->help == 2) {
            free(ui->details);
            ui->details = r->balance;
            r->balance = NULL;
        }
        if (r->cycle.path) {
            Entry *e = gui_selected(ui->pane + 1);
            if (e && !strcmp(e->path, r->cycle.path)) {
                entry_free(e);
                *e = r->cycle;
                memset(&r->cycle, 0, sizeof(r->cycle));
            }
        }
        char *after = gui_selection_signature(ui);
        if (strcmp(before, after)) {
            ui->preview_version = -1;
            gui_request_preview(ui, 0);
        }
        free(before);
        free(after);
    }
    view_result_free(r);
}

void gui_change_page(Gui *ui, int side, int page, int end)
{
    GuiPane *p = ui->pane + side;
    p->page = page;
    p->scroll_pending = 0;
    free(p->anchor);
    p->anchor = NULL;
    p->page_end = end;
    p->selected = p->offset = 0;
    entries_free(p->rows, p->n);
    free(p->marked);
    p->rows = NULL;
    p->marked = NULL;
    p->n = 0;
    gui_pane_reload(ui, side);
    if (side == ui->inspection_side)
        gui_request_preview(ui, 1);
}

void gui_scroll_selection(Gui *ui, GuiPane *p)
{
    int visible = gui_table_visible(ui, p == ui->pane ? 0 : 1);
    if (p->selected < p->offset)
        p->offset = p->selected;
    if (p->selected >= p->offset + visible)
        p->offset = p->selected - visible + 1;
}

void gui_navigate(Gui *ui, int side, const char *path)
{
    if (!valid_relative(path, 1)) {
        gui_notice(ui, "Enter a relative path without ..");
        return;
    }
    GuiPane *p = ui->pane + side;
    char *destination = xstrdup(path);
    activity_log(ui->c, LOG_DEBUG, "navigation.change", p->path, destination,
                 "Pane %d: leaving page %d, row %d, scroll %d", side, p->page, p->selected,
                 p->offset);
    gui_navigation_save(ui, side);
    gui_navigation_restore(ui, side, destination, p->path);
    gui_clear_marks(p);
    entries_free(p->rows, p->n);
    free(p->marked);
    p->rows = NULL;
    p->marked = NULL;
    p->n = 0;
    free(p->path);
    p->path = destination;
    p->total = 0;
    if (side)
        gui_refresh_folder(ui);
    gui_pane_reload(ui, side);
    if (side == ui->inspection_side)
        gui_request_preview(ui, 1);
}

void gui_enter_directory(Gui *ui, int side)
{
    GuiPane *p = ui->pane + side;
    if (*p->path && p->selected == 0) {
        char *par = parent_path(p->path);
        gui_navigate(ui, side, par);
        free(par);
        return;
    }
    Entry *e = gui_selected(p);
    if (e && !strcmp(e->kind, "directory")) {
        char *path = xstrdup(e->path);
        gui_navigate(ui, side, path);
        free(path);
    } else if (e) {
        gui_set_focus(ui, side);
        ui->focus = 2;
        gui_request_preview(ui, 1);
    }
}

void gui_go_parent(Gui *ui, int side)
{
    char *par = parent_path(ui->pane[side].path);
    gui_navigate(ui, side, par);
    free(par);
}

void gui_seek_row(Gui *ui, int side, int64_t row)
{
    GuiPane *p = &ui->pane[side];
    int64_t maximum = p->total - gui_table_visible(ui, side);
    if (maximum < 0)
        maximum = 0;
    if (row < 0)
        row = 0;
    if (row > maximum)
        row = maximum;
    int page = (int)(row / VIEW_PAGE_SIZE), within = (int)(row % VIEW_PAGE_SIZE);
    if (page != p->page) {
        gui_change_page(ui, side, page, 0);
        p->scroll_pending = 1;
        p->scroll_row = within;
    } else if (!p->n && (ui->view_dirty & (side ? VIEW_RIGHT : VIEW_LEFT))) {
        p->scroll_pending = 1;
        p->scroll_row = within;
    } else {
        p->offset = row == 0 ? 0 : within + (*p->path ? 1 : 0);
        int last = gui_row_count(p) - gui_table_visible(ui, side);
        if (p->offset > last)
            p->offset = last > 0 ? last : 0;
    }
}

const char *gui_sort_name(int column)
{
    static const char *names[] = {"Name", "Status / copies", "Size", "Modified", "Created", "Mode"};
    return names[column >= 0 && column < SORT_COUNT ? column : SORT_NAME];
}

void gui_set_sort(Gui *ui, int side, int column, int descending)
{
    if (column < 0 || column >= SORT_COUNT)
        return;
    GuiPane *p = &ui->pane[side];
    Entry *e = gui_selected(p);
    char *anchor = e ? xstrdup(e->path) : NULL;
    free(p->anchor);
    p->anchor = anchor;
    p->sort = column | (descending ? SORT_DESC : 0);
    if (column == SORT_MTIME || column == SORT_BTIME)
        p->created = column == SORT_BTIME;
    p->page = p->offset = p->selected = p->page_end = p->scroll_pending = 0;
    gui_pane_reload(ui, side);
    gui_save_ui(ui);
    activity_log(ui->c, LOG_DEBUG, "view.sort", p->path, NULL, "Pane %d: %s %s", side,
                 gui_sort_name(column), descending ? "descending" : "ascending");
}

int gui_column_at(Gui *ui, int side, float x)
{
    GuiTable t = gui_table(ui, side);
    if (x < t.body.x || x >= t.body.x + t.body.w)
        return -1;
    float at = x - t.body.x + ui->pane[side].horizontal;
    for (int i = 0; i < 5; i++) {
        if (at < t.widths[i])
            return i == 3   ? (ui->pane[side].created ? SORT_BTIME : SORT_MTIME)
                   : i == 4 ? SORT_MODE
                            : i;
        at -= t.widths[i];
    }
    return -1;
}
