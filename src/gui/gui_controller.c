#include "gui_controller.h"
#include "gui_activity.h"
#include "gui_browser.h"
#include "gui_choices.h"
#include "gui_columns.h"
#include "gui_fonts.h"
#include "gui_jobs.h"
#include "gui_open.h"
#include "gui_state.h"
#include "gui_theme.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Commands, prompts, notices and persistence of view preferences. */
static const char *help_text =
    "KEYBOARD / CCurate\n"
    "\n"
    "Tab                 switch left / right pane\n"
    "Arrows, PgUp/PgDn   navigate; Home / End\n"
    "Enter / Backspace   open directory / go up\n"
    "Ctrl+Enter          open file in its default application\n"
    "Insert / Space      select files (multiple selection)\n"
    "*                   invert selection on this page\n"
    "F5                  copy from RIGHT to LEFT\n"
    "Ctrl+F5             preserve complete directory\n"
    "F6                  move / rename on DESTINATION\n"
    "Ctrl+Delete         remove selected DESTINATION copy (confirmation)\n"
    "F7                  new directory on DESTINATION\n"
    "F8 / Shift+F8       reject / restore source entry\n"
    "Ctrl+P              toggle reviewed flag\n"
    "Ctrl+F              show unsecured entries only\n"
    "Ctrl+H              source / archive history\n"
    "Ctrl+R              refresh tree and changed files\n"
    "Ctrl+Shift+R        rehash the entire source\n"
    "Ctrl+A              SHA-512 audit of destination\n"
    "Ctrl+B              deduplication statistics and balance\n"
    "Ctrl+E              find relocated destination files\n"
    "F3                  focus preview; arrows scroll\n"
    "Ctrl+D              focus details and occurrences\n"
    "Ctrl+Alt+PgUp/PgDn  pages of file occurrences\n"
    "[ / ]               PDF page / video position (+10 s)\n"
    "Ctrl+X              hex / format preview\n"
    "Ctrl+T              date column: modified / created\n"
    "Ctrl+V              next retained version in history\n"
    "/                   filter names in the active directory\n"
    "Ctrl+G              go to a relative directory path\n"
    "Ctrl+F3 / Ctrl+F6   sort by name / size\n"
    "Ctrl+S / Ctrl+Shift+S choose sort column / reverse order\n"
    "Ctrl+Alt+C          choose a column and set its width\n"
    "Ctrl+Alt+W          fit all columns to visible rows\n"
    "Alt+Left/Right      pan the active file table\n"
    "Ctrl++ / Ctrl+-     UI scale 50-400%; Ctrl+0: 100%\n"
    "Ctrl+,              set an exact UI scale\n"
    "F2                  all actions (mouse or keyboard)\n"
    "Ctrl+L              show / hide Activity log\n"
    "Ctrl+Shift+L        filter log level\n"
    "Ctrl+Shift+D        focus Activity log\n"
    "Ctrl+Shift+End      toggle follow latest log entries\n"
    "Ctrl+Alt+T          choose color scheme\n"
    "Ctrl+Alt+F          choose a monospace font\n"
    "Ctrl+Alt+L          set panel proportions\n"
    "Ctrl+Alt+0          reset panel layout\n"
    "Log: arrows / PgUp / PgDn scroll; Left/Right pan.\n"
    "Ctrl+PgUp/PgDn      previous / next file-list page\n"
    "F1 / F9             help / text command\n"
    "Esc                 close dialog / cancel operation\n"
    "F10                 save session and exit\n"
    "\n"
    "F9: refresh, scan, fullscan, audit, stats, balance, reconcile, export,\n"
    "history, filter, review, unreview, restore, hex, help, scale 200,\n"
    "log, logfilter, logfollow, themes, fonts, legend, open, sort, reverse,\n"
    "columns, autofit, resetcolumns, remove.\n"
    "Click a column header to sort; click again to reverse.\n"
    "Drag a header divider to resize; double-click it to fit visible rows.\n"
    "Scrollbars: drag thumb or click track; Shift+wheel pans.\n"
    "Preview and details follow the active file pane (click or Tab).\n"
    "Destination inspection reads its actual file and metadata, with\n"
    "indexed source links. F3 / Ctrl+D keep that pane's context.\n"
    "F3 again or Esc returns to that file pane.\n"
    "Preview/details: Left/Right pan, Home/End scroll vertically.\n"
    "Drag any separator to resize panes. Double-click to reset.\n"
    "Ctrl+click selects files. Every action is in the bottom menu.\n"
    "Double-click a file to open its default application.\n"
    "External applications may modify the opened file.\n"
    "\n"
    "F8 never deletes data from the source drive.\n"
    "Remove destination copy operates on one indexed regular file.\n"
    "A live source copy must pass SHA-512 verification; otherwise removal\n"
    "is refused. All linked occurrences become unsecured; history stays.\n"
    "The removal dialog defaults to Cancel. Tab switches buttons.\n"
    "A complete directory preserves all retained paths\n"
    "in history, including empty directories. Each unique\n"
    "content still has only one physical destination copy.\n"
    "The physical directory may omit duplicates stored\n"
    "elsewhere. Symlinks and special objects retain their\n"
    "metadata; they are not materialized on the destination.\n";

void gui_notice(Gui *ui, const char *s)
{
    free(ui->message);
    ui->message = xstrdup(s);
    Rwidget_invalidate(ui->canvas);
}

void gui_prompt(Gui *ui, int mode, const char *title, const char *text)
{
    if (gui_operation_busy(ui) && (mode == 1 || mode == 2)) {
        gui_notice(ui, "Wait for the operation to finish.");
        return;
    }
    ui->dialog = mode;
    float width = fminf(540, ui->width - 24);
    Rwidget_set_size(ui->prompt_box, width, 210);
    Rwidget_set_visibility(ui->prompt_label, R_VISIBLE);
    Rwidget_set_visibility(ui->remove_scroll, R_COLLAPSED);
    Rwidget_set_size(ui->prompt_label, -1, -1);
    Rlabel_set_wrap(ui->prompt_label, 0);
    Rwidget_set_visibility(ui->input, R_VISIBLE);
    Rbutton_set_text(ui->prompt_confirm, "Confirm (Enter)");
    Rbutton_set_text(ui->prompt_cancel, "Cancel (Esc)");
    Rwidget_set_tooltip(ui->prompt_confirm, "Apply the value entered above.");
    Rwidget_set_size(ui->input, width - 60, 40);
    Rlabel_set_text(ui->prompt_label, title);
    Rtext_input_set_text(ui->input, text ? text : "");
    Rtext_input_select_all(ui->input);
    Rwidget_set_visibility(ui->prompt_box, R_VISIBLE);
    Rwidget_focus(ui->input);
}

void gui_close_prompt(Gui *ui)
{
    ui->dialog = 0;
    Rwidget_set_visibility(ui->prompt_box, R_COLLAPSED);
    Rwidget_focus(ui->canvas);
}

void gui_remove_focus(Gui *ui, int confirm)
{
    ui->remove_confirm = confirm;
    Rbutton_set_text(ui->prompt_confirm, confirm ? "Remove copy (Enter)" : "Remove copy");
    Rbutton_set_text(ui->prompt_cancel, confirm ? "Cancel (Esc)" : "Cancel (Enter / Esc)");
    Rwidget_focus(confirm ? ui->prompt_confirm : ui->prompt_cancel);
}

void gui_remove_prompt(Gui *ui)
{
    if (gui_operation_busy(ui)) {
        gui_notice(ui, "Wait for the operation to finish before removing a destination copy.");
        return;
    }
    if (ui->focus != 0) {
        gui_notice(ui, "Select a file in DESTINATION first. Source files cannot be removed.");
        return;
    }
    Entry *e = gui_selected(ui->pane);
    if (!e || strcmp(e->kind, "file") || !*e->state || descendant(e->path, META_DIR)) {
        gui_notice(ui, "Select one indexed regular destination file; folders cannot be removed.");
        return;
    }
    if (ui->c->readonly || ui->c->sfd < 0) {
        gui_notice(ui, "Connect the live source before removing a destination copy.");
        return;
    }
    free(ui->prompt_source);
    ui->prompt_source = xstrdup(e->path);
    char *path = display_path(e->path);
    char *label = fmt("Remove destination copy?\n\n%s\n\n"
                      "One file will be removed from DESTINATION.\n"
                      "Its linked source paths will become unsecured.\n"
                      "Source files, history and reviewed flags stay intact.\n\n"
                      "Removal requires an identical live source copy (SHA-512).\n"
                      "Cancel is selected initially.\n"
                      "Tab switches buttons; (Enter) marks the active action.\n"
                      "Up/Down or PgUp/PgDn scroll this message.",
                      path);
    gui_prompt(ui, 9, label, "");
    float width = fminf(760, ui->width - 24);
    float height = fminf(360, ui->height - 24);
    Rwidget_set_size(ui->prompt_box, width, height);
    Rwidget_set_visibility(ui->prompt_label, R_COLLAPSED);
    Rwidget_set_size(ui->remove_scroll, width - 48, height - 100);
    Rwidget_set_size(ui->remove_label, width - 68, -1);
    Rlabel_set_text(ui->remove_label, label);
    Rscroll_box_set_offset(ui->remove_scroll, 0, 0);
    Rwidget_set_visibility(ui->remove_scroll, R_VISIBLE);
    Rwidget_set_visibility(ui->input, R_COLLAPSED);
    Rwidget_set_tooltip(ui->prompt_confirm,
                        "Verify both copies with SHA-512, then remove only the destination file.");
    gui_remove_focus(ui, 0);
    free(label);
    free(path);
}

void gui_command(Gui *ui, const char *s)
{
    if (!strncmp(s, "scale ", 6)) {
        double value = atof(s + 6);
        if (isfinite(value) && value >= 50 && value <= 400) {
            ui->scale = value / 100;
            Rset_ui_scale(ui->scale);
            gui_save_ui(ui);
        } else
            gui_notice(ui, "Use: scale 200 (range 50-400).");
    } else if (!strcmp(s, "sort")) {
        ui->sort_side = ui->inspection_side;
        gui_dropdown_open(ui, 3);
    } else if (!strcmp(s, "columns")) {
        ui->column_side = ui->inspection_side;
        gui_dropdown_open(ui, 4);
    } else if (!strcmp(s, "autofit")) {
        gui_columns_autofit(ui, ui->inspection_side, -1);
    } else if (!strcmp(s, "resetcolumns")) {
        gui_columns_reset(ui, ui->inspection_side);
    } else if (!strcmp(s, "reverse")) {
        int side = ui->inspection_side;
        int sort = ui->pane[side].sort;
        gui_set_sort(ui, side, sort & SORT_COLUMN_MASK, !(sort & SORT_DESC));
    } else if (!strcmp(s, "open"))
        gui_open_selected(ui);
    else if (!strcmp(s, "remove"))
        gui_remove_prompt(ui);
    else if (!strcmp(s, "scan"))
        gui_start_job(ui, "scan", NULL, NULL, 0);
    else if (!strcmp(s, "fullscan"))
        gui_start_job(ui, "scan", NULL, NULL, 1);
    else if (!strcmp(s, "refresh"))
        gui_refresh_folder(ui);
    else if (!strcmp(s, "audit") || !strcmp(s, "reconcile") || !strcmp(s, "export"))
        gui_start_job(ui, s, NULL, NULL, 0);
    else if (!strcmp(s, "balance") || !strcmp(s, "stats")) {
        free(ui->details);
        ui->details = xstrdup(gui_operation_busy(ui)
                                  ? "Statistics will update after the current operation finishes.\n"
                                    "You can keep browsing."
                                  : "Calculating deduplication statistics in the background...\n"
                                    "This reads the catalog; source files are not rehashed.\n"
                                    "Large catalogs can take a while. You can keep browsing.");
        ui->details_scroll = 0;
        ui->focus = 3;
        ui->help = 2;
        gui_request_view(ui, VIEW_BALANCE);
    } else if (!strcmp(s, "history")) {
        ui->history = !ui->history;
        gui_refresh_folder(ui);
        gui_clear_marks(ui->pane + 1);
        ui->pane[1].page = 0;
        gui_pane_reload(ui, 1);
        gui_request_preview(ui, 1);
    } else if (!strcmp(s, "filter")) {
        ui->filter = !ui->filter;
        ui->pane[1].page = 0;
        gui_pane_reload(ui, 1);
        gui_request_preview(ui, 1);
    } else if (!strcmp(s, "review") || !strcmp(s, "unreview"))
        gui_mark_selected(ui, s);
    else if (!strcmp(s, "restore"))
        gui_mark_selected(ui, "restore");
    else if (!strcmp(s, "hex")) {
        ui->hex = !ui->hex;
        ui->preview_version = -1;
        gui_request_preview(ui, 0);
    } else if (!strcmp(s, "log")) {
        ui->log_visible = !ui->log_visible;
        if (ui->log_visible) {
            ui->focus = 4;
            gui_refresh_log(ui);
        } else if (ui->focus == 4)
            gui_set_focus(ui, ui->inspection_side);
        gui_save_ui(ui);
    } else if (!strcmp(s, "logfilter")) {
        ui->log_visible = 1;
        ui->focus = 4;
        gui_dropdown_open(ui, 1);
        gui_save_ui(ui);
    } else if (!strcmp(s, "logfollow")) {
        ui->log_visible = 1;
        ui->log_follow = !ui->log_follow;
        ui->focus = 4;
        gui_refresh_log(ui);
        gui_save_ui(ui);
    } else if (!strcmp(s, "fonts")) {
        gui_font_picker_open(ui);
    } else if (!strcmp(s, "themes")) {
        gui_dropdown_open(ui, 2);
    } else if (!strcmp(s, "legend")) {
        free(ui->details);
        ui->details = xstrdup(gui_theme_legend());
        ui->details_scroll = 0;
        ui->focus = 3;
        ui->help = 1;
    } else if (!strcmp(s, "help")) {
        free(ui->details);
        ui->details = fmt("%s\n%s", help_text, gui_theme_legend());
        ui->details_scroll = 0;
        ui->focus = 3;
        ui->help = 1;
    } else
        gui_notice(ui, "Unknown command. Press F1 for the list.");
}

void gui_prompt_accept(Gui *ui)
{
    char *text = xstrdup(Rtext_input_get_text(ui->input));
    int mode = ui->dialog;
    gui_close_prompt(ui);
    if (mode == 9)
        gui_start_job(ui, "remove", ui->prompt_source, NULL, 0);
    else if (mode == 1) {
        char *path = join_path(ui->pane[0].path, text);
        if (valid_relative(path, 0) && !descendant(path, META_DIR))
            gui_start_job(ui, "mkdir", path, NULL, 0);
        else
            gui_notice(ui, "Invalid path.");
        free(path);
    } else if (mode == 2) {
        if (valid_relative(text, 0))
            gui_start_job(ui, "move", ui->prompt_source, text, 0);
        else
            gui_notice(ui, "Invalid relative path.");
    } else if (mode == 3) {
        int side = ui->inspection_side;
        free(ui->pane[side].search);
        ui->pane[side].search = xstrdup(text);
        ui->pane[side].page = 0;
        gui_pane_reload(ui, side);
        gui_request_preview(ui, 1);
    } else if (mode == 4)
        gui_navigate(ui, ui->inspection_side, text);
    else if (mode == 5)
        gui_command(ui, text);
    else if (mode == 6) {
        char *end;
        double value = strtod(text, &end);
        if (end != text && !*end && isfinite(value) && value >= 50 && value <= 400) {
            ui->scale = value / 100;
            Rset_ui_scale(ui->scale);
            gui_save_ui(ui);
        } else
            gui_notice(ui, "Scale: enter a number from 50 to 400.");
    } else if (mode == 8) {
        char *end;
        double width = strtod(text, &end);
        if (!strcmp(text, "auto"))
            gui_columns_autofit(ui, ui->column_side, ui->column_index);
        else if (end != text && !*end && isfinite(width) && width >= GUI_COLUMN_MIN &&
                 width <= GUI_COLUMN_MAX) {
            gui_column_set_width(ui, ui->column_side, ui->column_index, (float)width);
            gui_save_ui(ui);
        } else
            gui_notice(ui,
                       "Column width: enter 32-32768 logical pixels, or auto to fit visible rows.");
    } else if (mode == 7) {
        float a, b, c, d;
        char extra;
        int parsed = sscanf(text, "%f %f %f %f %c", &a, &b, &c, &d, &extra);
        if (parsed == 4 && isfinite(a) && isfinite(b) && isfinite(c) && isfinite(d) && a >= 15 &&
            a <= 85 && b >= 15 && b <= 85 && c >= 15 && c <= 85 && d >= 15 && d <= 85) {
            ui->split_top = a / 100;
            ui->split_bottom = b / 100;
            ui->split_rows = c / 100;
            ui->split_log = d / 100;
            gui_save_ui(ui);
        } else
            gui_notice(ui, "Layout: enter four percentages from 15 to 85 (example: 50 50 53 25).");
    }
    free(text);
}

void gui_layout_prompt(Gui *ui)
{
    char value[80];
    snprintf(value, sizeof(value), "%.0f %.0f %.0f %.0f", ui->split_top * 100,
             ui->split_bottom * 100, ui->split_rows * 100, ui->split_log * 100);
    gui_prompt(ui, 7, "Layout %: left | preview | upper | log (15-85)", value);
}

void gui_save_ui(Gui *ui)
{
    char widths[2][128];
    for (int i = 0; i < 2; i++) {
        const float *w = ui->pane[i].column_widths;
        snprintf(widths[i], sizeof(widths[i]), "%.1f %.1f %.1f %.1f %.1f", w[0], w[1], w[2], w[3],
                 w[4]);
    }
    if (ui->view)
        view_save(ui->view, ui->pane[0].path, ui->pane[1].path, ui->pane[1].selected, ui->scale,
                  ui->filter, ui->pane[1].page, ui->split_top, ui->split_bottom, ui->split_rows,
                  ui->theme, ui->log_visible, ui->log_level, ui->split_log, ui->font_file,
                  ui->font_family, ui->font_style, ui->font_index, ui->pane[0].sort,
                  ui->pane[1].sort, ui->pane[0].created, ui->pane[1].created, widths[0], widths[1]);
}
