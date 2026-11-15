#include "gui_details.h"
#include "gui_browser.h"
#include "gui_controller.h"
#include "gui_render.h"
#include "gui_state.h"
#include "gui_theme.h"
#include <math.h>
#include <string.h>

static GuiRect button(Gui *ui, int index)
{
    GuiRect pane = gui_geometry(ui).pane[3];
    float width = fmaxf(1, pane.w - 24);
    float toggle = pane.w < 160 ? width : fminf(175, fmaxf(50, width - 80));
    return (GuiRect){pane.x + 12 + (index ? toggle + 5 + (index - 1) * 35 : 0), pane.y + 35,
                     index ? 30 : toggle, 26};
}

static int enabled(Gui *ui, int index)
{
    return ui->detail_history.available &&
           (!index || (ui->detail_history.expanded &&
                       (index == 1 ? ui->detail_history.page > 0 : ui->detail_history.more)));
}

GuiRect gui_details_body(Gui *ui)
{
    GuiRect pane = gui_geometry(ui).pane[3];
    float top = ui->help || pane.h < 94 ? 40 : 70;
    return (GuiRect){pane.x + 12, pane.y + top, fmaxf(1, pane.w - 32), fmaxf(1, pane.h - top - 18)};
}

void gui_details_paint(Gui *ui, RPainter *p)
{
    if (ui->help)
        return;
    GuiRect pane = gui_geometry(ui).pane[3];
    if (pane.h < 94)
        return;
    Rdraw_push_clip_rect(p, pane.x + 2, pane.y + 2, pane.w - 4, pane.h - 4);
    for (int i = 0; i < (pane.w < 160 ? 1 : 3); i++) {
        GuiRect r = button(ui, i);
        const char *label = i == 1                        ? "<"
                            : i == 2                      ? ">"
                            : r.w < 150                   ? "Versions"
                            : ui->detail_history.expanded ? "Hide history"
                                                          : "Version history";
        Rdraw_rect(p, r.x, r.y, r.w, r.h, 0, GUI_COLOR(ui, raised), 1);
        if (enabled(ui, i) && ui->history_hover == i + 1)
            Rdraw_rect_outline(p, r.x, r.y, r.w, r.h, 0, 1, GUI_COLOR(ui, accent), 1);
        gui_text_clip(ui, p, r.x + 6, r.y + 4, r.w - 12, r.h - 6, label,
                      GUI_HEX_COLOR(enabled(ui, i) ? gui_theme_current(ui)->text
                                                   : gui_theme_current(ui)->dim),
                      ui->font_size - 2);
    }
    if (pane.w >= 160 && ui->detail_history.expanded && ui->detail_history.available) {
        GuiRect last = button(ui, 2);
        char label[40];
        snprintf(label, sizeof(label), "Page %d", ui->detail_history.page + 1);
        gui_text_clip(ui, p, last.x + last.w + 8, last.y + 4,
                      pane.x + pane.w - last.x - last.w - 20, 22, label, GUI_COLOR(ui, muted),
                      ui->font_size - 2);
    }
    Rdraw_pop_clip_rect(p);
}

int gui_details_command(Gui *ui, const char *command)
{
    int index = !strcmp(command, "versions")       ? 0
                : !strcmp(command, "prevversions") ? 1
                : !strcmp(command, "nextversions") ? 2
                                                   : -1;
    if (index < 0)
        return 0;
    if (!enabled(ui, index)) {
        gui_notice(ui,
                   index ? "No further history page." : "No version history for this selection.");
        return 1;
    }
    if (!index) {
        ui->detail_history.expanded = !ui->detail_history.expanded;
        ui->detail_history.page = 0;
    } else
        ui->detail_history.page += index == 1 ? -1 : 1;
    ui->history_seek = 1;
    ui->help = 0;
    ui->focus = 3;
    ui->details_horizontal = 0;
    gui_request_view(ui, VIEW_DETAILS);
    activity_log(ui->c, LOG_DEBUG, "view.versions", ui->preview_path, NULL,
                 "%s history %s; page %d", ui->inspection_side ? "Source" : "Destination",
                 ui->detail_history.expanded ? "expanded" : "collapsed",
                 ui->detail_history.page + 1);
    return 1;
}

int gui_details_event(Gui *ui, const REvent *event)
{
    if (ui->tab)
        return 0;
    int index = -1;
    GuiRect pane = gui_geometry(ui).pane[3];
    if (!ui->help && pane.h >= 94)
        for (int i = 0; i < (pane.w < 160 ? 1 : 3); i++)
            if (gui_contains(button(ui, i), event->x, event->y))
                index = i;
    if (event->type == R_EVENT_MOUSE_MOVE)
        ui->history_hover = index + 1;
    if (event->type == R_EVENT_KEY_DOWN &&
        (event->modifiers & (R_MOD_CTRL | R_MOD_SHIFT | R_MOD_ALT)) == (R_MOD_CTRL | R_MOD_SHIFT)) {
        if (event->key == R_KEY_V)
            index = 0;
        else if (event->key == R_KEY_PAGE_UP)
            index = 1;
        else if (event->key == R_KEY_PAGE_DOWN)
            index = 2;
        else
            return 0;
    } else if (event->type != R_EVENT_MOUSE_DOWN || event->button != R_MOUSE_LEFT || index < 0)
        return 0;
    const char *commands[] = {"versions", "prevversions", "nextversions"};
    return gui_details_command(ui, commands[index]);
}

const char *gui_details_tip(Gui *ui)
{
    if (ui->help || ui->tab || gui_geometry(ui).pane[3].h < 94 || !ui->history_hover)
        return NULL;
    const char *tips[] = {"Show / hide recorded metadata and content changes | Ctrl+Shift+V",
                          "Previous version-history page | Ctrl+Shift+PgUp",
                          "Next version-history page | Ctrl+Shift+PgDn"};
    return tips[ui->history_hover - 1];
}

void gui_details_loaded(Gui *ui)
{
    if (!ui->history_seek || !ui->details)
        return;
    ui->history_seek = 0;
    const char *start = strstr(ui->details, "\nVERSION HISTORY:");
    if (!start)
        return;
    ui->details_scroll = 0;
    for (const char *p = ui->details; p <= start; p++)
        ui->details_scroll += *p == '\n';
}
