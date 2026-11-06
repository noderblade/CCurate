#include "gui_navigation.h"
#include "gui_browser.h"
#include "gui_state.h"
#include <stdlib.h>
#include <string.h>

struct GuiLocation {
    char *path, *selected_path, *search;
    int page, selected, offset, history, filter, sort;
    GuiLocation *next;
};

static void location_free(GuiLocation *location)
{
    free(location->path);
    free(location->selected_path);
    free(location->search);
    free(location);
}

static int matches(Gui *ui, int side, GuiLocation *location, const char *path)
{
    GuiPane *p = ui->pane + side;
    return !strcmp(location->path, path) && !strcmp(location->search, p->search) &&
           location->sort == p->sort && location->history == (side && ui->history) &&
           location->filter == (side && ui->filter);
}

void gui_navigation_save(Gui *ui, int side)
{
    GuiPane *p = ui->pane + side;
    if (p->anchor || (!p->rows && (ui->view_dirty & (side ? VIEW_RIGHT : VIEW_LEFT))))
        return;
    GuiLocation **link = &p->locations;
    while (*link) {
        if (matches(ui, side, *link, p->path)) {
            GuiLocation *old = *link;
            *link = old->next;
            location_free(old);
            break;
        }
        link = &(*link)->next;
    }
    Entry *entry = gui_selected(p);
    GuiLocation *location = xmalloc(sizeof(*location));
    *location = (GuiLocation){.path = xstrdup(p->path),
                              .selected_path = xstrdup(entry ? entry->path : ""),
                              .search = xstrdup(p->search),
                              .page = p->page,
                              .selected = p->selected,
                              .offset = p->offset,
                              .history = side && ui->history,
                              .filter = side && ui->filter,
                              .sort = p->sort,
                              .next = p->locations};
    p->locations = location;
    int count = 0;
    link = &p->locations;
    while (*link) {
        if (++count > 128) {
            GuiLocation *old = *link;
            *link = old->next;
            location_free(old);
        } else
            link = &(*link)->next;
    }
}

void gui_navigation_restore(Gui *ui, int side, const char *path, const char *previous)
{
    GuiPane *p = ui->pane + side;
    free(p->anchor);
    p->anchor = NULL;
    p->page = p->selected = p->offset = p->page_end = p->scroll_pending = 0;
    for (GuiLocation *location = p->locations; location; location = location->next)
        if (matches(ui, side, location, path)) {
            p->page = location->page;
            p->selected = location->selected;
            p->offset = location->offset;
            if (*location->selected_path)
                p->anchor = xstrdup(location->selected_path);
            break;
        }
    char *parent = parent_path(previous);
    if (*previous && !strcmp(parent, path)) {
        free(p->anchor);
        p->anchor = xstrdup(previous);
    }
    free(parent);
}

void gui_navigation_free(GuiPane *p)
{
    free(p->anchor);
    p->anchor = NULL;
    while (p->locations) {
        GuiLocation *old = p->locations;
        p->locations = old->next;
        location_free(old);
    }
}
