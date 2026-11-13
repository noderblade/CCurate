#include "gui_browser.h"
#include "gui_controller.h"
#include "gui_jobs.h"
#include "gui_state.h"
#include "gui_tabs.h"
#include "gui_usage_state.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static const float default_widths[USAGE_COLUMNS] = {280, 120, 88, 120, 128, 94, 126};

void gui_usage_init(Gui *ui)
{
    GuiUsage *u = xmalloc(sizeof(*u));
    ui->usage = u;
    u->request.path = xstrdup("");
    u->request.selection = xstrdup("");
    u->request.sort = USAGE_BYTES;
    u->request.descending = 1;
    u->request.depth = 3;
    u->split = .55f;
    u->details_split = .25f;
    u->columns = (1u << USAGE_COLUMNS) - 1;
    u->selected = -1;
    u->hover = -1;
    memcpy(u->widths, default_widths, sizeof(default_widths));
    char *saved = setting(&ui->reader, "ui.usage", "");
    if (*saved) {
        int tab = 0, sort = USAGE_BYTES, desc = 1, keep = 0, depth = 3, map = 0, page = 0,
            offset = 0, horizontal = 0;
        unsigned columns = 127;
        float split = .55f, details = .25f;
        char *line = strchr(saved, '\n');
        int n = sscanf(saved, "%d %d %d %d %d %d %d %d %d %f %f %u", &tab, &sort, &desc, &keep,
                       &depth, &map, &page, &offset, &horizontal, &split, &details, &columns);
        if (n == 12 && line) {
            ui->tab = !!tab;
            u->request.sort = sort >= 0 && sort < USAGE_COLUMNS ? sort : USAGE_BYTES;
            u->request.descending = !!desc;
            u->request.keep = !!keep;
            u->request.depth = depth >= 1 && depth <= 5 ? depth : 3;
            u->treemap = !!map;
            u->request.page = page >= 0 ? page : 0;
            u->offset = offset >= 0 ? offset : 0;
            u->horizontal = horizontal >= 0 ? horizontal : 0;
            if (isfinite(split) && split >= .2f && split <= .8f)
                u->split = split;
            if (isfinite(details) && details >= .15f && details <= .6f)
                u->details_split = details;
            u->columns = (columns & 127) | 1;
            char *width_line = line + 1, *path_line = strchr(width_line, '\n');
            if (path_line) {
                for (int i = 0; i < USAGE_COLUMNS; i++) {
                    char *end;
                    float width = strtof(width_line, &end);
                    if (end == width_line)
                        break;
                    if (isfinite(width) && width >= 55 && width <= 2000)
                        u->widths[i] = width;
                    width_line = end;
                }
                char *selection_line = strchr(path_line + 1, '\n');
                if (selection_line) {
                    *selection_line = 0;
                    size_t len = 0;
                    unsigned char *path = unb64(path_line + 1, &len);
                    if (path && valid_relative((char *)path, 1)) {
                        free(u->request.path);
                        u->request.path = xstrdup((char *)path);
                    }
                    free(path);
                    char *expanded_line = strchr(selection_line + 1, '\n');
                    if (expanded_line)
                        *expanded_line++ = 0;
                    path = unb64(selection_line + 1, &len);
                    if (path && valid_relative((char *)path, 1)) {
                        free(u->request.selection);
                        u->request.selection = xstrdup((char *)path);
                    }
                    free(path);
                    while (expanded_line && *expanded_line &&
                           u->request.expanded_count < USAGE_EXPAND_LIMIT) {
                        char *next = strchr(expanded_line, '\n');
                        if (next)
                            *next++ = 0;
                        path = unb64(expanded_line, &len);
                        if (path && valid_relative((char *)path, 0))
                            u->request.expanded[u->request.expanded_count++] =
                                xstrdup((char *)path);
                        free(path);
                        expanded_line = next;
                    }
                }
            }
        }
    }
    free(saved);
}
void gui_usage_save(Gui *ui)
{
    GuiUsage *u = ui->usage;
    if (!u || !u->service)
        return;
    Buf b = {0};
    badd(&b, "%d %d %d %d %d %d %d %d %d %.4f %.4f %u\n", ui->tab, u->request.sort,
         u->request.descending, u->request.keep, u->request.depth, u->treemap, u->request.page,
         u->offset, u->horizontal, u->split, u->details_split, u->columns);
    for (int i = 0; i < USAGE_COLUMNS; i++)
        badd(&b, "%.1f%s", u->widths[i], i == USAGE_COLUMNS - 1 ? "\n" : " ");
    char *path = b64(u->request.path, strlen(u->request.path)),
         *selection = b64(u->request.selection, strlen(u->request.selection));
    badd(&b, "%s\n%s", path, selection);
    for (int i = 0; i < u->request.expanded_count; i++) {
        char *expanded = b64(u->request.expanded[i], strlen(u->request.expanded[i]));
        badd(&b, "\n%s", expanded);
        free(expanded);
    }
    free(path);
    free(selection);
    usage_save(u->service, b.s);
    bfree(&b);
}
void gui_usage_request(Gui *ui)
{
    GuiUsage *u = ui->usage;
    if (!u || !u->service)
        return;
    u->serial = usage_submit(u->service, &u->request);
    u->loading = 1;
    free(u->error);
    u->error = NULL;
    gui_usage_save(ui);
}
void gui_usage_activate(Gui *ui)
{
    GuiUsage *u = ui->usage;
    if (!u->service)
        u->service = usage_start(ui->c);
    if (!u->service) {
        gui_notice(ui, "Cannot start disk usage worker.");
        return;
    }
    gui_usage_request(ui);
    gui_usage_tick(ui);
}
void gui_usage_keep_visible(Gui *ui)
{
    GuiUsage *u = ui->usage;
    int n = u->result ? u->result->count : 0, visible = gui_usage_geometry(ui).visible;
    if (u->selected >= n)
        u->selected = n - 1;
    if (u->selected >= 0 && u->selected < u->offset)
        u->offset = u->selected;
    if (u->selected >= u->offset + visible)
        u->offset = u->selected - visible + 1;
    if (u->offset > n - visible)
        u->offset = n - visible;
    if (u->offset < 0)
        u->offset = 0;
}
void gui_usage_tick(Gui *ui)
{
    GuiUsage *u = ui->usage;
    if (!u || !u->service)
        return;
    int updating = atomic_load(&ui->busy) && ui->job &&
                   (!strcmp(ui->job, "scan") || !strcmp(ui->job, "refresh"));
    usage_pause(u->service, updating);
    usage_status(u->service, &u->status);
    if (u->status.phase == USAGE_CANCELLED || u->status.phase == USAGE_ERROR)
        u->loading = 0;
    UsageResult *result = usage_take(u->service);
    if (!result)
        return;
    if (result->serial != u->serial) {
        usage_result_free(result);
        return;
    }
    u->loading = 0;
    if (result->error) {
        free(u->error);
        u->error = xstrdup(result->error);
        usage_result_free(result);
        return;
    }
    usage_result_free(u->result);
    u->result = result;
    u->request.page = result->page;
    u->selected = -1;
    u->hover = -1;
    for (int i = 0; i < result->count; i++)
        if (!strcmp(result->rows[i].path, u->request.selection)) {
            u->selected = i;
            break;
        }
    gui_usage_keep_visible(ui);
    u->details_lines = 1;
    u->details_width = 0;
    const char *p = result->details;
    while (p && *p) {
        const char *end = strchr(p, '\n');
        char *line = end ? strndup(p, end - p) : xstrdup(p);
        float width = 0;
        Rfont_measure_text(ui->font, ui->font_size, line, &width, NULL);
        free(line);
        u->details_width = fmaxf(u->details_width, width);
        if (!end)
            break;
        u->details_lines++;
        p = end + 1;
    }
    activity_log(ui->c, LOG_DEBUG, "usage.view", result->path, NULL,
                 "Logical=%lld unique=%lld repeated=%lld; %d rows; revision %lld",
                 (long long)result->totals.bytes,
                 (long long)(result->totals.hashed - result->totals.repeated),
                 (long long)result->totals.repeated, result->count, (long long)result->revision);
}
void gui_usage_select(Gui *ui, int row)
{
    GuiUsage *u = ui->usage;
    if (!u->result || row < 0 || row >= u->result->count)
        return;
    u->selected = row;
    free(u->request.selection);
    u->request.selection = xstrdup(u->result->rows[row].path);
    u->request.occurrence_page = 0;
    u->details_scroll = u->details_horizontal = 0;
    u->help = 0;
    gui_usage_keep_visible(ui);
    gui_usage_request(ui);
}
void gui_usage_navigate(Gui *ui, const char *path)
{
    GuiUsage *u = ui->usage;
    if (!valid_relative(path, 1))
        return;
    char *next = xstrdup(path);
    GuiUsageLocation *save = NULL;
    for (int i = 0; i < USAGE_LOCATIONS; i++)
        if (u->locations[i].path && !strcmp(u->locations[i].path, u->request.path)) {
            save = u->locations + i;
            break;
        }
    if (!save)
        save = u->locations + (u->location_next++ % USAGE_LOCATIONS);
    free(save->path);
    free(save->selection);
    save->path = xstrdup(u->request.path);
    save->selection = xstrdup(u->request.selection);
    save->page = u->request.page;
    save->offset = u->offset;
    save->horizontal = u->horizontal;
    free(u->request.path);
    u->request.path = next;
    u->request.page = 0;
    u->offset = u->horizontal = 0;
    free(u->request.selection);
    u->request.selection = xstrdup("");
    u->selected = -1;
    for (int i = 0; i < USAGE_LOCATIONS; i++)
        if (u->locations[i].path && !strcmp(u->locations[i].path, next)) {
            GuiUsageLocation *old = u->locations + i;
            u->request.page = old->page;
            u->offset = old->offset;
            u->horizontal = old->horizontal;
            free(u->request.selection);
            u->request.selection = xstrdup(old->selection);
            break;
        }
    u->help = 0;
    u->details_scroll = u->details_horizontal = 0;
    u->request.occurrence_page = 0;
    gui_usage_request(ui);
}
void gui_usage_expand(Gui *ui, int row)
{
    GuiUsage *u = ui->usage;
    if (!u->result || row < 0 || row >= u->result->count)
        return;
    UsageRow *r = u->result->rows + row;
    if (strcmp(r->kind, "directory"))
        return;
    if (r->more) {
        gui_usage_navigate(ui, r->path);
        return;
    }
    for (int i = 0; i < u->request.expanded_count; i++)
        if (!strcmp(u->request.expanded[i], r->path)) {
            free(u->request.expanded[i]);
            memmove(u->request.expanded + i, u->request.expanded + i + 1,
                    (u->request.expanded_count - i - 1) * sizeof(char *));
            u->request.expanded_count--;
            gui_usage_request(ui);
            return;
        }
    if (u->request.expanded_count == USAGE_EXPAND_LIMIT) {
        free(u->request.expanded[0]);
        memmove(u->request.expanded, u->request.expanded + 1,
                (USAGE_EXPAND_LIMIT - 1) * sizeof(char *));
        u->request.expanded_count--;
    }
    u->request.expanded[u->request.expanded_count++] = xstrdup(r->path);
    gui_usage_request(ui);
}
static void reveal(Gui *ui)
{
    GuiUsage *u = ui->usage;
    const char *path = *u->request.selection ? u->request.selection : u->request.path;
    char *anchor = xstrdup(path), *parent = parent_path(path);
    ui->history = ui->filter = 0;
    free(ui->pane[1].search);
    ui->pane[1].search = xstrdup("");
    gui_navigate(ui, 1, parent);
    free(parent);
    free(ui->pane[1].anchor);
    ui->pane[1].anchor = anchor;
    gui_request_view(ui, VIEW_RIGHT);
    gui_set_focus(ui, 1);
    gui_tabs_select(ui, 0);
}
int gui_usage_command(Gui *ui, const char *command)
{
    GuiUsage *u = ui->usage;
    if (!u)
        return 0;
    if (!strcmp(command, "curation")) {
        gui_tabs_select(ui, 0);
        return 1;
    }
    if (!strcmp(command, "usage")) {
        gui_tabs_select(ui, 1);
        return 1;
    }
    if (ui->tab && (!strcmp(command, "help") || !strcmp(command, "legend"))) {
        u->help = 1;
        u->details_scroll = u->details_horizontal = 0;
        return 1;
    }
    if (ui->tab) {
        if (!strcmp(command, "columns"))
            return gui_usage_command(ui, "usage.columns");
        if (!strcmp(command, "sort"))
            return gui_usage_command(ui, "usage.sort");
        if (!strcmp(command, "autofit"))
            return gui_usage_command(ui, "usage.fit");
        if (!strcmp(command, "reverse"))
            return gui_usage_command(ui, "usage.reverse");
        if (!strcmp(command, "open"))
            return gui_usage_command(ui, "usage.reveal");
        if (!strcmp(command, "remove") || !strcmp(command, "review") ||
            !strcmp(command, "unreview") || !strcmp(command, "restore") ||
            !strcmp(command, "filter") || !strcmp(command, "history") || !strcmp(command, "hex") ||
            !strcmp(command, "refresh") || !strcmp(command, "resetcolumns")) {
            gui_notice(ui, "Use Reveal in Curation to operate on the selected source entry.");
            return 1;
        }
    }
    if (strncmp(command, "usage.", 6))
        return 0;
    if (!ui->tab)
        gui_tabs_select(ui, 1);
    const char *action = command + 6;
    if (!strcmp(action, "root"))
        gui_usage_navigate(ui, "");
    else if (!strcmp(action, "up")) {
        char *path = parent_path(u->request.path);
        gui_usage_navigate(ui, path);
        free(path);
    } else if (!strcmp(action, "rings"))
        u->treemap = 0;
    else if (!strcmp(action, "treemap"))
        u->treemap = 1;
    else if (!strcmp(action, "columns")) {
        u->popup = 1;
        u->popup_row = 0;
    } else if (!strcmp(action, "sort")) {
        u->popup = 2;
        u->popup_row = u->request.sort;
    } else if (!strcmp(action, "fit"))
        gui_usage_fit(ui, -1);
    else if (!strcmp(action, "reverse")) {
        u->request.descending = !u->request.descending;
        gui_usage_request(ui);
    } else if (!strcmp(action, "scope")) {
        u->request.keep = !u->request.keep;
        u->request.page = 0;
        u->offset = 0;
        gui_usage_request(ui);
    } else if (!strcmp(action, "rebuild")) {
        u->loading = 1;
        free(u->error);
        u->error = NULL;
        usage_rebuild(u->service);
        gui_usage_request(ui);
    } else if (!strcmp(action, "cancel"))
        usage_cancel(u->service);
    else if (!strcmp(action, "reveal"))
        reveal(ui);
    else if (!strcmp(action, "depth+") || !strcmp(action, "depth-")) {
        u->request.depth += action[5] == '+' ? 1 : -1;
        if (u->request.depth < 1)
            u->request.depth = 1;
        if (u->request.depth > 5)
            u->request.depth = 5;
        gui_usage_request(ui);
    } else if (!strcmp(action, "previous") || !strcmp(action, "next")) {
        int page = u->request.page + (!strcmp(action, "next") ? 1 : -1);
        if (page >= 0 && u->result && (int64_t)page * USAGE_PAGE < u->result->children) {
            u->request.page = page;
            u->offset = 0;
            free(u->request.selection);
            u->request.selection = xstrdup("");
            gui_usage_request(ui);
        }
    } else if (!strcmp(action, "occurrences-") || !strcmp(action, "occurrences+")) {
        u->request.occurrence_page += strchr(action, '+') ? 1 : -1;
        if (u->request.occurrence_page < 0)
            u->request.occurrence_page = 0;
        u->details_scroll = 0;
        u->help = 0;
        gui_usage_request(ui);
    } else if (!strcmp(action, "layout")) {
        u->split = u->split < .5f ? .55f : u->split < .65f ? .7f : .4f;
    } else if (!strcmp(action, "resetlayout")) {
        u->split = .55f;
        u->details_split = .25f;
    }
    gui_usage_save(ui);
    return 1;
}
void gui_usage_scroll(Gui *ui, int id, double value)
{
    GuiUsage *u = ui->usage;
    if (id == 14) {
        u->offset = (int)value;
        u->focus = 0;
    } else if (id == 15) {
        u->horizontal = (int)value;
        u->focus = 0;
    } else if (id == 16) {
        u->details_scroll = (int)value;
        u->focus = 1;
    } else if (id == 17) {
        u->details_horizontal = (int)value;
        u->focus = 1;
    }
}
int gui_usage_popup(Gui *ui)
{
    return ui->usage && ui->usage->popup;
}
void gui_usage_shutdown(Gui *ui)
{
    GuiUsage *u = ui->usage;
    if (!u)
        return;
    gui_usage_save(ui);
    usage_stop(u->service);
    free(u->request.path);
    free(u->request.selection);
    for (int i = 0; i < u->request.expanded_count; i++)
        free(u->request.expanded[i]);
    for (int i = 0; i < USAGE_LOCATIONS; i++) {
        free(u->locations[i].path);
        free(u->locations[i].selection);
    }
    usage_result_free(u->result);
    free(u->error);
    free(u);
    ui->usage = NULL;
}
