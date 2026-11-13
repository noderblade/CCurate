#include "gui_menu.h"
#include "gui_browser.h"
#include "gui_controller.h"
#include "gui_input.h"
#include "gui_layout.h"
#include "gui_render.h"
#include "gui_scroll.h"
#include "gui_state.h"
#include "gui_theme.h"
#include "gui_usage.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Action definitions, footer menus and mouse/keyboard menu navigation. */
/* Canvas action menu: the same action definitions drive mouse and keyboard use.
 * Keep help inside the popup so it remains readable at large UI scales. */
typedef struct {
    int group;
    const char *label, *shortcut, *help;
    RKey key;
    Ruint modifiers;
    const char *command;
    int special;
} Action;
enum { CLEAR_FILTER = 1, CLEAR_SELECTION, FOCUS_LEFT, FOCUS_RIGHT, RESET_LAYOUT, SET_LAYOUT };
static const char *menu_groups[] = {"Files",   "Scan",   "View", "Selection",
                                    "Preview", "Layout", "Help"};
static const Action actions[] = {
    {0, "Open in default application", "Ctrl+Enter",
     "Open the selected file with its desktop default application. In history view, open the "
     "retained destination copy. External applications may modify the opened file.",
     0, 0, "open", 0},
    {0, "Copy to destination", "F5",
     "Copy the selected source files into the left directory; verify SHA-512 and retain all known "
     "duplicate paths.",
     R_KEY_F5, 0, NULL, 0},
    {0, "Keep complete directory", "Ctrl+F5",
     "Retain the selected directory tree, including empty folders and duplicate paths, with one "
     "physical copy per content.",
     R_KEY_F5, R_MOD_CTRL, NULL, 0},
    {0, "Move / rename destination", "F6",
     "Move the selected LEFT entry and update the archive index. Enter a path relative to the "
     "destination root.",
     R_KEY_F6, 0, NULL, 0},
    {0, "New destination directory", "F7", "Create a directory inside the current left pane.",
     R_KEY_F7, 0, NULL, 0},
    {0, "Reject source entry", "F8",
     "Exclude selected source entries from curation. This does not delete files on the source.",
     R_KEY_F8, 0, NULL, 0},
    {0, "Restore rejected entry", "Shift+F8",
     "Undo the rejected flag for the selected source entries.", R_KEY_F8, R_MOD_SHIFT, NULL, 0},
    {0, "Save session and exit", "F10",
     "Save pane locations, layout and scale; stop any worker safely and exit.", R_KEY_F10, 0, NULL,
     0},
    {0, "Remove destination copy", "Ctrl+Delete",
     "Select one indexed file in DESTINATION. Confirm removal after SHA-512 checks of both "
     "copies. Refuse if no identical live source remains. Linked paths become unsecured; "
     "source files, history and reviewed flags stay intact. Folders are not removed.",
     0, 0, "remove", 0},
    {1, "Refresh this folder", "F9: refresh",
     "Check this source folder only. Reuse unchanged hashes and verify new or changed entries. "
     "Folders are also checked automatically when you enter them.",
     0, 0, "refresh", 0},
    {1, "Scan entire source", "Ctrl+R",
     "Discover new or changed entries and hash changed files. Source rows appear during "
     "enumeration.",
     0, 0, "scan", 0},
    {1, "Full SHA-512 rescan", "Ctrl+Shift+R",
     "Read and hash every regular source file again, even when its metadata is unchanged.", 0, 0,
     "fullscan", 0},
    {1, "Audit destination", "Ctrl+A",
     "Recompute SHA-512 for archived destination content and flag missing or changed copies.", 0, 0,
     "audit", 0},
    {1, "Find relocated copies", "Ctrl+E",
     "Search the destination for missing archived content and update its indexed location.", 0, 0,
     "reconcile", 0},
    {1, "Export archive JSONL", "F9: export",
     "Regenerate the readable archive index from retained content and all its recorded original "
     "paths.",
     0, 0, "export", 0},
    {1, "Cancel current operation", "Esc",
     "Stop the background operation; completed verified copies and scan observations stay saved.",
     R_KEY_ESCAPE, 0, NULL, 0},
    {2, "Source / archive history", "Ctrl+H",
     "Toggle the right pane between the live source and retained original paths backed by "
     "destination copies.",
     0, 0, "history", 0},
    {2, "Show unsecured only", "Ctrl+F",
     "Toggle the filter for entries still needing attention; rejected entries are excluded.", 0, 0,
     "filter", 0},
    {2, "Filter names", "/",
     "Filter the active directory by name across all list pages. An empty value clears the filter.",
     R_KEY_SLASH, 0, NULL, 0},
    {2, "Clear name filter", "", "Show all names in the active directory.", 0, 0, NULL,
     CLEAR_FILTER},
    {2, "Go to directory", "Ctrl+G",
     "Enter a directory relative to the active drive root. Leave empty to go to its root.", R_KEY_G,
     R_MOD_CTRL, NULL, 0},
    {2, "Open selected directory", "Enter",
     "Open the selected directory in the active pane; a file focuses that pane's preview.",
     R_KEY_ENTER, 0, NULL, 0},
    {2, "Parent directory", "Backspace", "Move up one directory in the active file pane.",
     R_KEY_BACKSPACE, 0, NULL, 0},
    {2, "Sort by name", "Ctrl+F3",
     "Sort the whole active directory by name, including entries on other pages.", R_KEY_F3,
     R_MOD_CTRL, NULL, 0},
    {2, "Sort by size", "Ctrl+F6", "Sort the whole active directory by descending size.", R_KEY_F6,
     R_MOD_CTRL, NULL, 0},
    {2, "Previous list page", "Ctrl+PgUp",
     "Load the previous page of up to 1,000 entries in the active pane.", R_KEY_PAGE_UP, R_MOD_CTRL,
     NULL, 0},
    {2, "Next list page", "Ctrl+PgDn",
     "Load the next page of up to 1,000 entries in the active pane.", R_KEY_PAGE_DOWN, R_MOD_CTRL,
     NULL, 0},
    {2, "Modified / created date", "Ctrl+T",
     "Switch the date column. Creation time is shown only when the filesystem provides it.",
     R_KEY_T, R_MOD_CTRL, NULL, 0},
    {2, "Activity log", "Ctrl+L",
     "Show or hide the resizable Activity panel below preview and details. The full history is "
     "saved in .ccurate/activity.log.",
     0, 0, "log", 0},
    {2, "Filter activity log", "Ctrl+Shift+L",
     "Choose All, Info, Warnings, Errors or Debug. Filtering affects only the panel; every level "
     "is saved to the file.",
     0, 0, "logfilter", 0},
    {2, "Focus activity log", "Ctrl+Shift+D",
     "Focus the log; arrows and Page Up/Down scroll. Left/Right pans long records.", R_KEY_D,
     R_MOD_CTRL | R_MOD_SHIFT, NULL, 0},
    {2, "Follow latest log entries", "Ctrl+Shift+End",
     "Toggle automatic scrolling to new entries. Manual scrolling pauses following.", 0, 0,
     "logfollow", 0},
    {2, "Sort by column", "Ctrl+S",
     "Choose any column and direction for the whole active directory. Click headers to sort or "
     "reverse.",
     0, 0, "sort", 0},
    {2, "Reverse sort order", "Ctrl+Shift+S",
     "Reverse the active pane's sort direction. Directories remain first; selection follows its "
     "path.",
     0, 0, "reverse", 0},
    {3, "Select / deselect file", "Space",
     "Toggle selection of the highlighted file. Ctrl+click also toggles a row.", R_KEY_SPACE, 0,
     NULL, 0},
    {3, "Invert selection on page", "*",
     "Invert the selection on this list page; selections on other pages stay unchanged.",
     R_KEY_KP_MULTIPLY, 0, NULL, 0},
    {3, "Clear selection", "",
     "Clear all selected files in the active pane, including other pages.", 0, 0, NULL,
     CLEAR_SELECTION},
    {3, "Toggle reviewed", "Ctrl+P",
     "Toggle the independent reviewed flag on selected source entries. Reviewing does not copy or "
     "reject them.",
     R_KEY_P, R_MOD_CTRL, NULL, 0},
    {3, "Mark reviewed", "F9: review", "Set the reviewed flag for every selected source entry.", 0,
     0, "review", 0},
    {3, "Mark unreviewed", "F9: unreview",
     "Clear the reviewed flag for every selected source entry.", 0, 0, "unreview", 0},
    {3, "Focus destination", "Tab", "Activate the left file pane.", 0, 0, NULL, FOCUS_LEFT},
    {3, "Focus source", "Tab", "Activate the right file pane.", 0, 0, NULL, FOCUS_RIGHT},
    {4, "Focus preview", "F3",
     "Inspect the active source or destination file. Arrow keys scroll; F3 returns to that pane.",
     R_KEY_F3, 0, NULL, 0},
    {4, "Focus details / occurrences", "Ctrl+D",
     "Focus file metadata, duplicate locations and archived destination information.", R_KEY_D,
     R_MOD_CTRL, NULL, 0},
    {4, "Format / hex preview", "Ctrl+X",
     "Toggle between a format-specific preview and a hexadecimal view.", 0, 0, "hex", 0},
    {4, "Previous PDF page / frame", "[",
     "Show the previous PDF page or move the video preview back ten seconds.", R_KEY_LEFT_BRACKET,
     0, NULL, 0},
    {4, "Next PDF page / frame", "]",
     "Show the next PDF page or move the video preview forward ten seconds.", R_KEY_RIGHT_BRACKET,
     0, NULL, 0},
    {4, "Previous occurrences", "Ctrl+Alt+PgUp",
     "Show the previous page of duplicate and historical source locations.", R_KEY_PAGE_UP,
     R_MOD_CTRL | R_MOD_ALT, NULL, 0},
    {4, "Next occurrences", "Ctrl+Alt+PgDn",
     "Show the next page of duplicate and historical source locations.", R_KEY_PAGE_DOWN,
     R_MOD_CTRL | R_MOD_ALT, NULL, 0},
    {4, "Next retained version", "Ctrl+V",
     "Cycle older retained content versions for the selected entry in history view.", R_KEY_V,
     R_MOD_CTRL, NULL, 0},
    {5, "Set UI scale", "Ctrl+,",
     "Enter an exact scale from 50 to 400 percent. The complete interface scales together.",
     R_KEY_COMMA, R_MOD_CTRL, NULL, 0},
    {5, "Increase UI scale", "Ctrl++",
     "Increase the complete interface scale by 12.5 percentage points, up to 400%.", R_KEY_EQUAL,
     R_MOD_CTRL, NULL, 0},
    {5, "Decrease UI scale", "Ctrl+-",
     "Decrease the complete interface scale by 12.5 percentage points, down to 50%.", R_KEY_MINUS,
     R_MOD_CTRL, NULL, 0},
    {5, "Reset UI scale", "Ctrl+0", "Return the interface to 100 percent.", R_KEY_0, R_MOD_CTRL,
     NULL, 0},
    {5, "Set panel proportions", "Ctrl+Alt+L",
     "Enter four percentages: destination width, preview width, upper row height, log height. You "
     "can also "
     "drag the separators.",
     0, 0, NULL, SET_LAYOUT},
    {5, "Reset panel layout", "Ctrl+Alt+0",
     "Restore equal pane widths and the default row and log heights. Double-click a pane separator "
     "to "
     "reset "
     "only that split.",
     0, 0, NULL, RESET_LAYOUT},
    {5, "Color scheme", "Ctrl+Alt+T",
     "Choose a light or dark palette, including Dracula, Gruvbox Dark, Catppuccin Mocha and Tokyo "
     "Night. The selected theme is saved in "
     "the session.",
     0, 0, "themes", 0},
    {5, "Monospace font", "Ctrl+Alt+F",
     "Choose an installed monospace family and style, preview it, and apply it to the entire "
     "interface. The choice is saved in the session.",
     0, 0, "fonts", 0},
    {5, "Set column width", "Ctrl+Alt+C",
     "Choose a column in the active file pane, then enter its width in logical pixels or auto. "
     "You can also drag header dividers. Widths are saved separately for each pane.",
     0, 0, "columns", 0},
    {5, "Fit columns to visible rows", "Ctrl+Alt+W",
     "Fit every column in the active file pane to its header and visible rows. Double-click a "
     "header divider to fit just that column. Off-screen rows are not measured.",
     0, 0, "autofit", 0},
    {5, "Reset column widths", "F9: resetcolumns",
     "Restore responsive default column widths in the active file pane.", 0, 0, "resetcolumns", 0},
    {6, "Keyboard and safety help", "F1",
     "Show all keyboard shortcuts and the source-data preservation rules in the information panel.",
     0, 0, "help", 0},
    {6, "Statistics / balance", "Ctrl+B",
     "Show logical size before/after deduplication, saved bytes, remaining data, unscanned files "
     "and curation progress. Unique* on the status bar means incomplete scan coverage.",
     0, 0, "balance", 0},
    {6, "Text command", "F9",
     "Enter refresh, scan, fullscan, audit, stats, balance, reconcile, export, history, filter, "
     "review, unreview, "
     "restore, hex, log, logfilter, logfollow, themes, fonts, legend, open, sort, reverse, "
     "columns, autofit, resetcolumns, help or scale 200.",
     R_KEY_F9, 0, NULL, 0},
    {6, "File status legend", "",
     "Explain every status, duplicate marker, reviewed/selection prefix and file color.", 0, 0,
     "legend", 0},
    {2, "Disk Usage workspace", "Ctrl+Tab",
     "Analyze logical source sizes, duplicate content and directory proportions from the saved "
     "catalog.",
     0, 0, "usage", 0},
    {2, "Curation workspace", "Ctrl+Tab", "Return to the saved curation panels and selections.", 0,
     0, "curation", 0},
    {2, "Usage: Rings chart", "R",
     "Show nested directory rings; each angle represents logical bytes.", 0, 0, "usage.rings", 0},
    {2, "Usage: Treemap chart", "T", "Show nested rectangles sized by logical bytes.", 0, 0,
     "usage.treemap", 0},
    {2, "Usage: parent directory", "Backspace",
     "Return to the parent, restoring its selection and scroll position.", 0, 0, "usage.up", 0},
    {2, "Usage: source root", "Ctrl+Home",
     "Show the whole catalog as the chart's 100 percent reference.", 0, 0, "usage.root", 0},
    {2, "Usage: fewer chart levels", "[", "Decrease visible chart depth, down to one ring.", 0, 0,
     "usage.depth-", 0},
    {2, "Usage: more chart levels", "]", "Increase visible chart depth, up to five levels.", 0, 0,
     "usage.depth+", 0},
    {2, "Usage: all / keep candidates", "Ctrl+F",
     "Include all present source entries, or exclude rejected files while keeping directory "
     "containers.",
     0, 0, "usage.scope", 0},
    {2, "Usage: visible columns", "Ctrl+Alt+C",
     "Choose size, share, unique, repeated, file count and unverified columns. Name stays visible.",
     0, 0, "usage.columns", 0},
    {2, "Usage: fit column widths", "Ctrl+Alt+W",
     "Fit all analysis columns to the displayed rows; double-click an edge to fit one column.", 0,
     0, "usage.fit", 0},
    {2, "Usage: sort column", "Ctrl+S", "Sort the whole current directory by any analysis column.",
     0, 0, "usage.sort", 0},
    {2, "Usage: reverse sort", "Ctrl+Shift+S", "Reverse the analysis table's sort direction.", 0, 0,
     "usage.reverse", 0},
    {2, "Usage: previous list page", "Ctrl+PgUp",
     "Show the previous bounded page of directory entries.", 0, 0, "usage.previous", 0},
    {2, "Usage: next list page", "Ctrl+PgDn",
     "Show the next bounded page; no entries are omitted from the browser.", 0, 0, "usage.next", 0},
    {2, "Usage: previous occurrences", "Ctrl+Alt+PgUp",
     "Show the previous page of matching current source files.", 0, 0, "usage.occurrences-", 0},
    {2, "Usage: next occurrences", "Ctrl+Alt+PgDn",
     "Show more matching source files, including paths outside the selected subtree.", 0, 0,
     "usage.occurrences+", 0},
    {2, "Usage: reveal in Curation", "F3",
     "Select the analyzed source path in Curation, retaining the destination folder.", 0, 0,
     "usage.reveal", 0},
    {2, "Usage: panel proportions", "Ctrl+Alt+L",
     "Cycle tree/chart proportions. You can also drag both analysis separators.", 0, 0,
     "usage.layout", 0},
    {2, "Usage: rebuild index", "",
     "Recompute the disposable analysis cache from SQLite. Source files are not read or hashed.", 0,
     0, "usage.rebuild", 0},
    {2, "Usage: cancel analysis", "Esc",
     "Cancel index preparation; the last completed generation remains saved.", 0, 0, "usage.cancel",
     0},

};

static int gui_action_count(Gui *ui);
static const Action *gui_menu_action(Gui *ui, int row);
static GuiRect gui_menu_rect(Gui *ui);
static int gui_menu_visible(GuiRect r);
static void gui_menu_keep_visible(Gui *ui);
static void gui_action_run(Gui *ui);
static int gui_footer_at(Gui *ui, float x, float y);

static int gui_action_count(Gui *ui)
{
    if (ui->menu == 1)
        return 7;
    int n = 0;
    for (unsigned i = 0; i < sizeof(actions) / sizeof(actions[0]); i++)
        if (actions[i].group == ui->menu - 2)
            n++;
    return n;
}

static const Action *gui_menu_action(Gui *ui, int row)
{
    for (unsigned i = 0; i < sizeof(actions) / sizeof(actions[0]); i++)
        if (actions[i].group == ui->menu - 2 && row-- == 0)
            return actions + i;
    return NULL;
}

void gui_menu_open(Gui *ui, int menu)
{
    ui->menu = menu;
    ui->menu_row = 0;
    ui->menu_offset = 0;
    ui->splitter_hover = 0;
    Rwidget_set_cursor(ui->canvas, R_CURSOR_ARROW);
}

static GuiRect gui_menu_rect(Gui *ui)
{
    float w = fminf(660, ui->width - 24),
          h = fminf(42 + gui_action_count(ui) * 29 + 100, ui->height - 52);
    float x = ui->menu < 2 ? 12 : 12 + (ui->menu - 1) * (ui->width - 24) / 8;
    x = fmaxf(12, fminf(ui->width - w - 12, x));
    return (GuiRect){x, ui->height - 42 - h, w, h};
}

static int gui_menu_visible(GuiRect r)
{
    return (int)fmaxf(1, floorf((r.h - 142) / 29));
}

static void gui_menu_keep_visible(Gui *ui)
{
    int visible = gui_menu_visible(gui_menu_rect(ui)), n = gui_action_count(ui);
    if (ui->menu_row < 0)
        ui->menu_row = 0;
    if (ui->menu_row >= n)
        ui->menu_row = n - 1;
    if (ui->menu_offset > ui->menu_row)
        ui->menu_offset = ui->menu_row;
    if (ui->menu_row >= ui->menu_offset + visible)
        ui->menu_offset = ui->menu_row - visible + 1;
}

static void gui_action_run(Gui *ui)
{
    if (ui->menu == 1) {
        gui_menu_open(ui, ui->menu_row + 2);
        return;
    }
    const Action *a = gui_menu_action(ui, ui->menu_row);
    ui->menu = 0;
    ui->footer_hover = -1;
    if (!a)
        return;
    if (ui->tab && a->special) {
        if (a->special == RESET_LAYOUT)
            gui_usage_command(ui, "usage.resetlayout");
        else
            gui_notice(ui, "Use the Disk Usage controls, or switch to Curation for file-pane actions.");
        return;
    }
    GuiPane *p = ui->pane + ui->inspection_side;
    if (a->command)
        gui_command(ui, a->command);
    else if (a->key) {
        if (ui->focus >= 2 && (a->key == R_KEY_ENTER || a->key == R_KEY_BACKSPACE ||
                               a->key == R_KEY_SPACE || a->key == R_KEY_KP_MULTIPLY))
            gui_set_focus(ui, ui->inspection_side);
        REvent e = {.type = R_EVENT_KEY_DOWN, .key = a->key, .modifiers = a->modifiers};
        gui_handle_event(ui, &e);
        ui->filter_opener = 0; /* No subsequent TEXT event for a menu click. */
    } else
        switch (a->special) {
        case CLEAR_FILTER:
            free(p->search);
            p->search = xstrdup("");
            p->page = 0;
            gui_pane_reload(ui, ui->inspection_side);
            break;
        case CLEAR_SELECTION:
            gui_clear_marks(p);
            break;
        case FOCUS_LEFT:
            gui_set_focus(ui, 0);
            break;
        case FOCUS_RIGHT:
            gui_set_focus(ui, 1);
            break;
        case RESET_LAYOUT:
            gui_reset_layout(ui);
            break;
        case SET_LAYOUT:
            gui_layout_prompt(ui);
            break;
        }
}

static int gui_footer_at(Gui *ui, float x, float y)
{
    if (x < 12 || x >= ui->width - 12 || y < ui->height - 34 || y >= ui->height - 4)
        return -1;
    return (int)((x - 12) / ((ui->width - 24) / (ui->width < 760 ? 3 : 8)));
}

void gui_menu_paint(Gui *ui, RPainter *p)
{
    int compact = ui->width < 760, count = compact ? 3 : 8;
    float bw = (ui->width - 24) / count;
    for (int i = 0; i < count; i++) {
        const char *label = compact ? (i == 0   ? "F2 Actions"
                                       : i == 1 ? "F5 Copy"
                                                : "F9 Commands")
                                    : (i == 0 ? "F2 Actions" : menu_groups[i - 1]);
        int hot = ui->footer_hover == i || (!compact && ui->menu == i + 1);
        Rdraw_rect(
            p, 12 + i * bw, ui->height - 34, bw - 4, 30, 4,
            GUI_HEX_COLOR(hot ? gui_theme_current(ui)->selection : gui_theme_current(ui)->raised),
            1);
        gui_text_clip(ui, p, 20 + i * bw, ui->height - 27, bw - 20, 22, label, GUI_COLOR(ui, text),
                      13);
    }
    if (!ui->menu) {
        const char *tip = NULL;
        if (ui->column_hover || ui->column_drag)
            tip =
                "Drag to resize column | Double-click to fit visible rows | Ctrl+Alt+C: set width";
        else if (ui->splitter_hover)
            tip = "Drag to resize | Double-click to reset | Ctrl+Alt+L: exact proportions";
        else if (ui->footer_hover >= 0)
            tip = compact ? (ui->footer_hover == 0
                                 ? "All actions: arrows to navigate, Enter to run, Esc to close."
                             : ui->footer_hover == 1
                                 ? "Copy source selection to the current destination directory."
                                 : "Open the text command dialog.")
                          : "Click for actions and hover descriptions | F2 opens the complete menu";
        if (tip) {
            float tw = fminf(720, ui->width - 24);
            Rdraw_rect(p, 12, ui->height - 100, tw, 60, 5, GUI_COLOR(ui, raised), 1);
            gui_multiline(ui, p, tip, 22, ui->height - 93, tw - 20, 50, 0);
        }
        return;
    }
    GuiRect r = gui_menu_rect(ui);
    int visible = gui_menu_visible(r), n = gui_action_count(ui);
    Rdraw_rect(p, r.x, r.y, r.w, r.h, 6, GUI_COLOR(ui, raised), 1);
    Rdraw_rect_outline(p, r.x, r.y, r.w, r.h, 6, 2, GUI_COLOR(ui, accent), 1);
    char title[120];
    snprintf(title, sizeof(title), "%s  |  %d/%d  |  Esc: close",
             ui->menu == 1 ? "ALL ACTIONS" : menu_groups[ui->menu - 2], ui->menu_row + 1, n);
    gui_text_clip(ui, p, r.x + 12, r.y + 10, r.w - 24, 24, title, GUI_COLOR(ui, accent), 14);
    for (int row = ui->menu_offset; row < n && row < ui->menu_offset + visible; row++) {
        float y = r.y + 40 + (row - ui->menu_offset) * 29;
        if (row == ui->menu_row)
            Rdraw_rect(p, r.x + 5, y, r.w - 10, 28, 3, GUI_COLOR(ui, selection), 1);
        const Action *a = ui->menu == 1 ? NULL : gui_menu_action(ui, row);
        const char *label = a ? a->label : menu_groups[row], *shortcut = a ? a->shortcut : ">";
        float right = a ? fminf(155, r.w * .32f) : 24;
        gui_text_clip(ui, p, r.x + 14, y + 5, r.w - right - 26, 24, label, GUI_COLOR(ui, text), 13);
        gui_text_clip(ui, p, r.x + r.w - right, y + 5, right - 8, 24, shortcut,
                      GUI_COLOR(ui, muted), 12);
    }
    float hy = r.y + r.h - 95;
    Rdraw_rect(p, r.x + 6, hy, r.w - 12, 89, 4, GUI_COLOR(ui, panel), 1);
    const Action *a = ui->menu == 1 ? NULL : gui_menu_action(ui, ui->menu_row);
    gui_multiline(
        ui, p,
        a ? a->help
          : "Open a category. Use arrows, Enter, Escape or the mouse. Scroll to reveal more "
            "actions.",
        r.x + 14, hy + 8, r.w - 28, 75, 0);
    gui_scrollbar_paint(ui, p, 12, (GuiRect){r.x + r.w - 14, r.y + 40, 10, visible * 29}, n,
                        visible, ui->menu_offset, 0);
}

int gui_menu_event(Gui *ui, const REvent *e)
{
    if (ui->drag)
        return 0;
    int footer = gui_footer_at(ui, e->x, e->y);
    if (e->type == R_EVENT_MOUSE_MOVE) {
        ui->footer_hover = footer;
        if (footer >= 0) {
            ui->splitter_hover = 0;
            Rwidget_set_cursor(ui->canvas, R_CURSOR_HAND);
        }
    }
    if (e->type == R_EVENT_MOUSE_DOWN && e->button == R_MOUSE_LEFT && footer >= 0) {
        if (ui->width < 760 && footer > 0) {
            ui->menu = 0;
            REvent k = {.type = R_EVENT_KEY_DOWN, .key = footer == 1 ? R_KEY_F5 : R_KEY_F9};
            gui_handle_event(ui, &k);
        } else if (ui->menu == footer + 1)
            ui->menu = 0;
        else
            gui_menu_open(ui, footer + 1);
        return 1;
    }
    if (!ui->menu)
        return footer >= 0 && e->type == R_EVENT_MOUSE_MOVE;
    if (e->type == R_EVENT_KEY_DOWN) {
        switch (e->key) {
        case R_KEY_ESCAPE:
        case R_KEY_F2:
            ui->menu = 0;
            break;
        case R_KEY_UP:
            ui->menu_row--;
            break;
        case R_KEY_DOWN:
            ui->menu_row++;
            break;
        case R_KEY_HOME:
            ui->menu_row = 0;
            break;
        case R_KEY_END:
            ui->menu_row = gui_action_count(ui) - 1;
            break;
        case R_KEY_PAGE_UP:
            ui->menu_row -= gui_menu_visible(gui_menu_rect(ui));
            break;
        case R_KEY_PAGE_DOWN:
            ui->menu_row += gui_menu_visible(gui_menu_rect(ui));
            break;
        case R_KEY_LEFT:
            if (ui->menu > 1)
                gui_menu_open(ui, 1);
            break;
        case R_KEY_RIGHT:
            if (ui->menu == 1)
                gui_action_run(ui);
            else
                gui_menu_open(ui, 2 + (ui->menu - 1) % 7);
            break;
        case R_KEY_ENTER:
        case R_KEY_KP_ENTER:
        case R_KEY_SPACE:
            gui_action_run(ui);
            break;
        default:
            break;
        }
        if (ui->menu)
            gui_menu_keep_visible(ui);
        return 1;
    }
    GuiRect r = gui_menu_rect(ui);
    if (e->type == R_EVENT_MOUSE_WHEEL) {
        ui->menu_row += e->wheel_y > 0 ? -3 : 3;
        gui_menu_keep_visible(ui);
        return 1;
    }
    if (e->type == R_EVENT_MOUSE_MOVE || e->type == R_EVENT_MOUSE_DOWN) {
        int row = (int)((e->y - r.y - 40) / 29) + ui->menu_offset;
        if (gui_contains(r, e->x, e->y) && e->y >= r.y + 40 && row >= ui->menu_offset &&
            row < gui_action_count(ui) && row < ui->menu_offset + gui_menu_visible(r)) {
            ui->menu_row = row;
            Rwidget_set_cursor(ui->canvas, R_CURSOR_HAND);
            if (e->type == R_EVENT_MOUSE_DOWN && e->button == R_MOUSE_LEFT)
                gui_action_run(ui);
        } else if (e->type == R_EVENT_MOUSE_DOWN && !gui_contains(r, e->x, e->y))
            ui->menu = 0;
        return 1;
    }
    return 1;
}
