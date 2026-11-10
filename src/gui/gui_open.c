#include "gui_open.h"
#include "gui_browser.h"
#include "gui_controller.h"
#include "gui_state.h"
#include <stdlib.h>
#include <string.h>

void gui_open_selected(Gui *ui)
{
    int side = ui->inspection_side;
    Entry *entry = gui_selected(ui->pane + side);
    if (!entry || !strcmp(entry->kind, "directory")) {
        gui_enter_directory(ui, side);
        return;
    }
    if (strcmp(entry->kind, "file")) {
        gui_notice(ui, "External opening is available for regular files. Symlinks and special "
                       "objects retain metadata only.");
        return;
    }
    if (ui->opener.pid > 0) {
        gui_notice(ui, "Waiting for the desktop to launch the previous file.");
        return;
    }
    int target = side == 0 || ui->history;
    const char *path = side && ui->history ? entry->dest : entry->path;
    free(ui->open_path);
    ui->open_path = join_path(target          ? ui->c->destination
                              : ui->c->source ? ui->c->source
                                              : "",
                              path);
    if (desktop_open_start(&ui->opener, target ? ui->c->dfd : ui->c->sfd, path)) {
        gui_notice(ui, ui->opener.message);
        activity_log(ui->c, LOG_ERROR, "open.failed", ui->open_path, NULL, "%s",
                     ui->opener.message);
    } else {
        gui_notice(ui, "Opening in the default application...");
        activity_log(ui->c, LOG_INFO, "open.request", ui->open_path, NULL,
                     "Requested desktop application launch");
    }
}

void gui_open_tick(Gui *ui)
{
    int result = desktop_open_poll(&ui->opener);
    if (!result)
        return;
    const char *message = result > 0 ? "Opened in the default application." : ui->opener.message;
    gui_notice(ui, message);
    activity_log(ui->c, result > 0 ? LOG_INFO : LOG_ERROR,
                 result > 0 ? "open.success" : "open.failed", ui->open_path, NULL, "%s", message);
}
