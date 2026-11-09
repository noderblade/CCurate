#include "gui.h"
#include "gui_activity.h"
#include "gui_browser.h"
#include "gui_controller.h"
#include "gui_fonts.h"
#include "gui_graphics.h"
#include "gui_input.h"
#include "gui_jobs.h"
#include "gui_navigation.h"
#include "gui_open.h"
#include "gui_render.h"
#include "gui_state.h"
#include "gui_theme.h"
#include "source_refresh.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include "platform/windows/diagnostics_windows.h"
#endif

/* Application lifecycle and Raven callback adapters. */

static void paint_callback(Rid widget, RPainter *p, float width, float height, void *data)
{
    (void)widget;
    gui_render(data, p, width, height);
}

static int event_callback(Rid window, const REvent *event, void *data)
{
    (void)window;
    return gui_handle_event(data, event);
}

static void confirm_callback(Rid widget, void *data)
{
    (void)widget;
    gui_prompt_accept(data);
}

static void cancel_callback(Rid widget, void *data)
{
    (void)widget;
    gui_close_prompt(data);
}

static void raven_error(RError code, RSeverity severity, const char *detail);

static void raven_error(RError code, RSeverity severity, const char *detail)
{
    fprintf(stderr, "Raven %d/%d: %s\n", code, severity, detail ? detail : "");
}

int gui_main(Catalog *c, int historical)
{
    Gui state = {0};
    Gui *ui = &state;
    ui->c = c;
    ui->focus = 1;
    ui->scroll_drag = -1;
    ui->history = historical;
    ui->font_size = 14;
    ui->row_height = 25;
    ui->texture = ui->font = ui->font_sample = R_INVALID_ID;
    ui->font_load_row = -1;
    ui->scale = 1;
    ui->split_top = ui->split_bottom = .5f;
    ui->split_rows = .53f;
    ui->split_log = .25f;
    ui->log_level = -1;
    ui->log_follow = 1;
    ui->theme = 1;
    ui->footer_hover = -1;
    ui->width = 1500;
    ui->height = 960;
    pthread_mutex_init(&ui->pm, NULL);
    pthread_cond_init(&ui->pc, NULL);
    char *db = join_path(c->state_dir, "catalog.sqlite");
    if (sqlite3_open_v2(db, &ui->reader.db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX, NULL) !=
        SQLITE_OK) {
        free(db);
        return fail(c, "GUI database");
    }
    free(db);
    sqlite3_busy_timeout(ui->reader.db, 1000);
    ui->reader.dfd = c->dfd;
    ui->reader.sfd = c->sfd;
    ui->reader.source = c->source;
    ui->reader.destination = c->destination;
    ui->reader.state_dir = c->state_dir;
    ui->pane[0].path = setting(&ui->reader, "ui.left", "");
    ui->pane[1].path = setting(&ui->reader, "ui.right", "");
    ui->pane[0].search = xstrdup("");
    ui->pane[1].search = xstrdup("");
    char *saved = setting(&ui->reader, "ui.selected", "0");
    ui->pane[1].selected = atoi(saved);
    free(saved);
    const char *sort_keys[] = {"ui.sort.left", "ui.sort.right"};
    const char *date_keys[] = {"ui.date.left", "ui.date.right"};
    for (int i = 0; i < 2; i++) {
        saved = setting(&ui->reader, sort_keys[i], "0");
        int sort = atoi(saved);
        free(saved);
        ui->pane[i].sort = sort >= 0 && (sort & SORT_COLUMN_MASK) < SORT_COUNT &&
                                   !(sort & ~(SORT_DESC | SORT_COLUMN_MASK))
                               ? sort
                               : SORT_NAME;
        saved = setting(&ui->reader, date_keys[i], "0");
        ui->pane[i].created = atoi(saved) != 0;
        free(saved);
    }
    saved = setting(&ui->reader, "ui.scale", "1");
    ui->scale = fmaxf(.5f, fminf(4, atof(saved)));
    free(saved);
    saved = setting(&ui->reader, "ui.page", "0");
    ui->pane[1].page = atoi(saved);
    free(saved);
    saved = setting(&ui->reader, "ui.filter", "0");
    ui->filter = atoi(saved);
    free(saved);
    const char *split_keys[] = {"ui.split.top", "ui.split.bottom", "ui.split.rows"};
    float *split_values[] = {&ui->split_top, &ui->split_bottom, &ui->split_rows};
    for (int i = 0; i < 3; i++) {
        saved = setting(&ui->reader, split_keys[i], i == 2 ? "0.53" : "0.5");
        float v = atof(saved);
        free(saved);
        if (isfinite(v) && v >= .15f && v <= .85f)
            *split_values[i] = v;
    }
    saved = setting(&ui->reader, "ui.theme", "1");
    ui->theme = atoi(saved);
    free(saved);
    if (ui->theme < 0 || ui->theme >= gui_theme_count())
        ui->theme = 1;
    saved = setting(&ui->reader, "ui.log.visible", "0");
    ui->log_visible = atoi(saved) != 0;
    free(saved);
    saved = setting(&ui->reader, "ui.log.level", "-1");
    ui->log_level = atoi(saved);
    free(saved);
    if (ui->log_level < -1 || ui->log_level > 3)
        ui->log_level = -1;
    saved = setting(&ui->reader, "ui.split.log", "0.25");
    float v = atof(saved);
    free(saved);
    if (isfinite(v) && v >= .15f && v <= .85f)
        ui->split_log = v;
    ui->font_file = setting(&ui->reader, "ui.font.file", "");
    ui->font_family = setting(&ui->reader, "ui.font.family", "");
    ui->font_style = setting(&ui->reader, "ui.font.style", "");
    saved = setting(&ui->reader, "ui.font.index", "0");
    ui->font_index = atoi(saved);
    free(saved);
    RInitDesc init = {0};
    init.backend = R_BACKEND_VULKAN;
    init.ui_scale = ui->scale;
    init.app_name = "CCurate";
    init.worker_threads = 2;
#ifdef _WIN32
    cc_win_startup_note("Initializing the curation workspace (Vulkan / display).");
#endif
    Rerror_set_callback(raven_error);
    if (!gui_graphics_init(&init)) {
        sqlite3_close(ui->reader.db);
        return fail(c, "Raven could not initialize Vulkan / display");
    }
    Rtheme_set_dark(1);
    Rtheme_set_accent(.22f, .62f, .85f);
    ui->font = Rfont_get_monospace();
    Rtheme_set_font(ui->font);
    ui->window = Rwindow_create("CCurate - manual archive curation", 1500, 960, R_WINDOW_DEFAULT);
    if (ui->window == R_INVALID_ID) {
        gui_graphics_shutdown();
        sqlite3_close(ui->reader.db);
        return fail(c, "Window creation failed");
    }
#ifdef _WIN32
    cc_win_startup_note("Curation window created; waiting for its first frame.");
#endif
    Rwindow_set_min_size(ui->window, 1000, 680);
    ui->overlay = Rwidget_create(R_WIDGET_OVERLAY);
    ui->canvas = Rwidget_create(R_WIDGET_CUSTOM);
    Rcustom_set_paint_callback(ui->canvas, paint_callback, ui);
    Rwidget_set_focusable(ui->canvas, 1);
    Rwidget_add_child(ui->overlay, ui->canvas);
    ui->prompt_box = Rwidget_create(R_WIDGET_BORDER);
    Rborder_set_background(ui->prompt_box, .10f, .14f, .20f, 1);
    Rborder_set_padding(ui->prompt_box, 24, 24, 24, 24);
    Rborder_set_outline(ui->prompt_box, 2, .25f, .70f, .95f, 1);
    Rwidget_set_size(ui->prompt_box, 540, 210);
    Rslot_set_halign(ui->prompt_box, R_HALIGN_CENTER);
    Rslot_set_valign(ui->prompt_box, R_VALIGN_CENTER);
    Rid box = Rwidget_create(R_WIDGET_VBOX);
    Rpanel_set_spacing(box, 20);
    ui->prompt_label = Rlabel_create("");
    Rwidget_add_child(box, ui->prompt_label);
    ui->input = Rwidget_create(R_WIDGET_TEXT_INPUT);
    Rwidget_set_size(ui->input, 480, 40);
    Rwidget_add_child(box, ui->input);
    Rid buttons = Rwidget_create(R_WIDGET_HBOX);
    Rpanel_set_spacing(buttons, 12);
    Rid confirm = Rbutton_create("Confirm (Enter)"), cancel = Rbutton_create("Cancel (Esc)");
    Rbutton_set_clicked_callback(confirm, confirm_callback, ui);
    Rbutton_set_clicked_callback(cancel, cancel_callback, ui);
    Rwidget_set_tooltip(confirm, "Apply the value entered above.");
    Rwidget_set_tooltip(cancel, "Close this dialog without applying changes.");
    Rwidget_add_child(buttons, confirm);
    Rwidget_add_child(buttons, cancel);
    Rwidget_add_child(box, buttons);
    Rwidget_add_child(ui->prompt_box, box);
    Rwidget_add_child(ui->overlay, ui->prompt_box);
    Rwidget_set_visibility(ui->prompt_box, R_COLLAPSED);
    gui_apply_theme(ui, ui->theme);
    Rwindow_set_content(ui->window, ui->overlay);
    Rwindow_set_event_hook(ui->window, event_callback, ui);
    Rwindow_set_tab_navigation(ui->window, 0);
    Rwidget_focus(ui->canvas);
    ui->view = view_start(c);
    if (!ui->view) {
        gui_graphics_shutdown();
        sqlite3_close(ui->reader.db);
        return fail(c, "Cannot start view worker");
    }
    /* Only a new catalog needs an automatic whole-tree inventory. Existing
     * sessions display saved pages immediately and refresh the visited folder. */
    if (!historical && source_needs_initial_scan(&ui->reader))
        gui_start_job(ui, "scan", NULL, NULL, 0);
    else if (!historical) {
        gui_refresh_folder(ui);
        char *complete = setting(&ui->reader, "scan_complete", "0");
        gui_notice(
            ui, !strcmp(complete, "1")
                    ? "Saved catalog loaded. Folders are checked as you browse; Ctrl+R scans all."
                    : "Saved catalog loaded; last full traversal was incomplete. Ctrl+R resumes "
                      "discovery.");
        free(complete);
    }
    gui_pane_reload(ui, 0);
    gui_pane_reload(ui, 1);
    gui_request_view(ui, VIEW_SUMMARY);
    gui_jobs_start_preview(ui);
    gui_request_preview(ui, 1);
    if (historical)
        gui_notice(ui, "History: files are read from the destination. Ctrl+H switches views.");
    ui->font_service = font_service_start(c);
    if (!ui->font_service)
        gui_notice(ui, "Font discovery could not start; using bundled monospace.");
    double last_refresh = 0, last_save = 0, last_log = 0;
    while (Rwait_frame(.04)) {
#ifdef _WIN32
        cc_win_startup_ready();
#endif
        if (atomic_load(&ui->finished)) {
            int lazy = gui_lazy_active(ui);
            pthread_join(ui->worker, NULL);
            atomic_store(&ui->finished, 0);
            atomic_store(&ui->busy, 0);
            char *s = lazy && atomic_load(&c->cancel)
                          ? xstrdup("Folder refresh stopped; saved catalog remains available.")
                      : ui->job_result ? fmt("Operation needs attention: %s",
                                             *c->error ? c->error : "check the audit")
                                       : fmt("Completed: %s", ui->job);
            gui_notice(ui, s);
            free(s);
            if (!lazy) {
                ui->target_refresh = 1;
                gui_pane_reload(ui, 0);
            }
            gui_pane_reload(ui, 1);
            gui_request_view(
                ui, VIEW_DETAILS | (!lazy || atomic_load(&c->refresh_changed) ? VIEW_SUMMARY : 0));
            gui_save_ui(ui);
        }
        gui_jobs_tick(ui);
        gui_apply_view(ui);
        gui_open_tick(ui);
        scan_status(c, &ui->scan);
        pthread_mutex_lock(&ui->pm);
        if (ui->ready_available) {
            ui->ready_available = 0;
            if (ui->result_seq == ui->request_seq) {
                preview_free(&ui->preview);
                ui->preview = ui->ready;
                memset(&ui->ready, 0, sizeof(ui->ready));
                if (ui->texture != R_INVALID_ID)
                    Rtexture_destroy(ui->texture);
                ui->texture = ui->preview.rgba
                                  ? Rtexture_create_from_pixels(ui->preview.rgba, ui->preview.width,
                                                                ui->preview.height)
                                  : R_INVALID_ID;
            } else
                preview_free(&ui->ready);
        }
        pthread_mutex_unlock(&ui->pm);
        double now = Rget_time();
        /* A long statistics/counter query must not hide newly committed folder
         * observations. Wait only for the preceding list request; replacing
         * that request mid-query would starve expensive sorts. */
        if (now - last_refresh > 1 && !(ui->view_dirty & VIEW_RIGHT)) {
            if (atomic_load(&ui->busy) && ui->job &&
                (!strcmp(ui->job, "scan") || !strcmp(ui->job, "refresh")) &&
                (ui->scan.phase == SCAN_ENUMERATE || ui->scan.phase == SCAN_HASH))
                gui_pane_reload(ui, 1);
            last_refresh = now;
        }
        gui_font_tick(ui);
        if (now - last_log > .25) {
            if (ui->log_visible)
                gui_refresh_log(ui);
            else
                activity_status(ui->c->log, ui->log_error);
            last_log = now;
        }
        if (!atomic_load(&ui->busy) && now - last_save > 15) {
            gui_save_ui(ui);
            last_save = now;
        }
        Rwidget_invalidate(ui->canvas);
    }
    gui_cancel_job(ui);
    if (atomic_load(&ui->busy)) {
        pthread_join(ui->worker, NULL);
    }
    gui_save_ui(ui);
    desktop_open_stop(&ui->opener);
    free(ui->open_path);
    gui_jobs_stop_preview(ui);
    view_stop(ui->view);
    ui->view = NULL;
    preview_free(&ui->ready);
    preview_free(&ui->preview);
    gui_font_shutdown(ui);
    gui_graphics_shutdown();
    for (int i = 0; i < 2; i++) {
        gui_clear_marks(ui->pane + i);
        gui_navigation_free(ui->pane + i);
        entries_free(ui->pane[i].rows, ui->pane[i].n);
        free(ui->pane[i].marked);
        free(ui->pane[i].path);
        free(ui->pane[i].search);
    }
    for (int i = 0; i < ui->selection_count; i++)
        free(ui->selections[i]);
    free(ui->selections);
    free(ui->job);
    free(ui->arg1);
    free(ui->arg2);
    activity_lines_free(ui->log_lines, ui->log_count);
    gui_text_cache_free(&ui->text_cache[0]);
    gui_text_cache_free(&ui->text_cache[1]);
    free(ui->details);
    free(ui->message);
    free(ui->prompt_source);
    free(ui->preview_path);
    sqlite3_close(ui->reader.db);
    pthread_mutex_destroy(&ui->pm);
    pthread_cond_destroy(&ui->pc);
    return 0;
}
