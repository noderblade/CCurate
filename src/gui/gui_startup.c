#include "gui.h"
#include <Raven.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef _WIN32
#include "platform/windows/dialog_windows.h"
#endif

/* The chooser runs before a catalog is opened. Cancelling it has no archive
 * side effects. Raven supplies its folder browser, without desktop helpers. */
typedef struct {
    Rid window, paths[2], buttons[2], start, error;
    char **roots[2];
    int historical, done, accepted;
} Startup;

static void show_error(Startup *s, const char *message)
{
    char *safe = display_path(message);
    Rlabel_set_text(s->error, safe);
    free(safe);
}

static void refresh(Startup *s)
{
    for (int i = 0; i < 2; i++) {
        char *safe = display_path(*s->roots[i] ? *s->roots[i] : "No folder selected");
        Rtext_input_set_text(s->paths[i], safe);
        Rwidget_set_tooltip(s->paths[i], safe);
        free(safe);
    }
    Rwidget_set_enabled(s->start, *s->roots[1] && (s->historical || *s->roots[0]));
}

static void browse(Rid button, void *data)
{
    Startup *s = data;
    int side = button == s->buttons[1];
    char *initial = *s->roots[side] ? xstrdup(*s->roots[side]) : NULL;
    if (!initial && side && *s->roots[0]) {
        initial = parent_path(*s->roots[0]);
        if (!*initial) {
            free(initial);
            initial = xstrdup("/");
        }
    }
    if (!initial)
        initial = getcwd(NULL, 0);
    RFileDialogDesc desc = {.parent = s->window,
                            .title =
                                side ? "Choose the destination folder" : "Choose the source folder",
                            .directory = initial,
                            .flags = R_FILE_DIALOG_RAVEN | R_FILE_DIALOG_SHOW_HIDDEN};
#ifdef _WIN32
    char *path = cc_win_choose_folder(s->window, desc.title, initial);
#else
    const char *path = Rfile_dialog_select_folder(&desc);
#endif
    if (path) {
        char *copy = xstrdup(path);
        free(*s->roots[side]);
        *s->roots[side] = copy;
        show_error(s, "");
        refresh(s);
    }
#ifdef _WIN32
    free(path);
#endif
    free(initial);
    if (Ris_running())
        Rwidget_focus(s->buttons[side]);
}

static void start(Rid button, void *data)
{
    (void)button;
    Startup *s = data;
    char *canonical[2] = {0};
    char *error = NULL;
    for (int i = s->historical ? 1 : 0; i < 2; i++) {
        const char *name = i ? "Destination" : "Source";
        if (!*s->roots[i]) {
            error = fmt("Choose a %s folder first.", i ? "destination" : "source");
            break;
        }
        canonical[i] = realpath(*s->roots[i], NULL);
        struct stat st;
        if (!canonical[i] || stat(canonical[i], &st)) {
            error = fmt("%s: %s", name, strerror(errno));
            break;
        }
        if (!S_ISDIR(st.st_mode)) {
            error = fmt("%s must be a directory.", name);
            break;
        }
    }
    /* Reject invalid pairs before catalog_open can create session files. */
    if (!error && !s->historical) {
        if (!strcmp(canonical[0], canonical[1]))
            error = xstrdup("Source and destination must be different folders.");
        else if (!strcmp(canonical[1], "/") || descendant(canonical[0], canonical[1]))
            error = xstrdup("The destination must not contain the source folder.");
    }
    if (error) {
        show_error(s, error);
        free(error);
    } else {
        for (int i = s->historical ? 1 : 0; i < 2; i++) {
            free(*s->roots[i]);
            *s->roots[i] = canonical[i];
            canonical[i] = NULL;
        }
        s->accepted = s->done = 1;
    }
    free(canonical[0]);
    free(canonical[1]);
}

static void cancel(Rid button, void *data)
{
    (void)button;
    ((Startup *)data)->done = 1;
}

static int event(Rid window, const REvent *e, void *data)
{
    Startup *s = data;
    if (Rwindow_has_dialog(window) || e->type != R_EVENT_KEY_DOWN)
        return 0;
    if (e->key == R_KEY_ESCAPE || e->key == R_KEY_F10) {
        cancel(R_INVALID_ID, s);
        return 1;
    }
    if ((e->modifiers & R_MOD_ALT) && (e->key == R_KEY_D || e->key == R_KEY_S)) {
        if (e->key == R_KEY_D || !s->historical)
            browse(s->buttons[e->key == R_KEY_D], s);
        return 1;
    }
    if ((e->modifiers & R_MOD_CTRL) && (e->key == R_KEY_ENTER || e->key == R_KEY_KP_ENTER)) {
        start(R_INVALID_ID, s);
        return 1;
    }
    return 0;
}

int gui_choose_roots(char **source, char **destination, int historical, const char *error)
{
    Startup s = {.roots = {source, destination}, .historical = historical};
    RInitDesc init = {.backend = R_BACKEND_VULKAN, .app_name = "CCurate", .worker_threads = 2};
    if (!Rinit_ex(&init)) {
        fputs("CCurate could not initialize the folder chooser (Vulkan / display).\n", stderr);
        return -1;
    }
    Rtheme_set_dark(1);
    Rtheme_set_font(Rfont_get_monospace());
    Rtheme_set_accent(.45f, .62f, .81f);
    s.window = Rwindow_create("CCurate - choose folders", 1000, 640, R_WINDOW_DEFAULT);
    if (s.window == R_INVALID_ID) {
        Rshutdown();
        return -1;
    }
    Rwindow_set_min_size(s.window, 860, 580);
    Rid border = Rwidget_create(R_WIDGET_BORDER), box = Rwidget_create(R_WIDGET_VBOX);
    Rborder_set_padding(border, 28, 24, 28, 24);
    Rpanel_set_spacing(box, 16);
    Rwidget_add_child(border, box);
    Rid title = Rlabel_create(historical ? "Open a curated archive" : "Choose your folders");
    Rlabel_set_text_size(title, 24);
    Rwidget_add_child(box, title);
    Rid hint = Rlabel_create(
        historical ? "Choose the destination containing your saved session."
                   : "Choose the old source and the destination for your curated files.");
    Rlabel_set_wrap(hint, 1);
    Rwidget_add_child(box, hint);
    for (int i = 0; i < 2; i++) {
        Rid label = Rlabel_create(i ? "DESTINATION - curated files and session"
                                    : "SOURCE - original files");
        Rwidget_add_child(box, label);
        Rid row = Rwidget_create(R_WIDGET_HBOX);
        Rpanel_set_spacing(row, 12);
        s.paths[i] = Rwidget_create(R_WIDGET_TEXT_INPUT);
        Rtext_input_set_read_only(s.paths[i], 1);
        Rslot_set_fill(s.paths[i], 1);
        Rwidget_set_size(s.paths[i], 0, 42);
        Rwidget_add_child(row, s.paths[i]);
        s.buttons[i] = Rbutton_create(i ? "Browse destination (Alt+D)" : "Browse source (Alt+S)");
        Rwidget_set_size(s.buttons[i], 286, 42);
        Rbutton_set_clicked_callback(s.buttons[i], browse, &s);
        Rwidget_set_tooltip(
            s.buttons[i], "Browse folders with the mouse or arrow keys; hidden folders are shown.");
        Rwidget_add_child(row, s.buttons[i]);
        Rwidget_add_child(box, row);
        if (historical && !i) {
            Rwidget_set_visibility(label, R_COLLAPSED);
            Rwidget_set_visibility(row, R_COLLAPSED);
        }
    }
    s.error = Rlabel_create("");
    Rlabel_set_wrap(s.error, 1);
    Rlabel_set_color(s.error, 1, .60f, .52f, 1);
    Rwidget_set_size(s.error, 0, 80);
    Rwidget_add_child(box, s.error);
    Rid keyboard = Rlabel_create("Tab: move focus  |  Ctrl+Enter: start  |  Esc: cancel");
    Rlabel_set_wrap(keyboard, 1);
    Rwidget_add_child(box, keyboard);
    Rid buttons = Rwidget_create(R_WIDGET_HBOX);
    Rpanel_set_spacing(buttons, 12);
    s.start = Rbutton_create(historical ? "Open archive" : "Start curation");
    Rbutton_set_clicked_callback(s.start, start, &s);
    Rid close = Rbutton_create("Cancel");
    Rbutton_set_clicked_callback(close, cancel, &s);
    Rwidget_set_size(s.start, 220, 42);
    Rwidget_set_size(close, 120, 42);
    Rwidget_add_child(buttons, s.start);
    Rwidget_add_child(buttons, close);
    Rwidget_add_child(box, buttons);
    refresh(&s);
    show_error(&s, error ? error : "");
    Rwindow_set_content(s.window, border);
    Rwindow_set_event_hook(s.window, event, &s);
    Rwidget_focus(s.buttons[historical || *source ? 1 : 0]);
    while (!s.done && Rwait_frame(.05))
        ;
    Rshutdown();
    return s.accepted;
}
