#include "gui_jobs.h"
#include "gui_browser.h"
#include "gui_controller.h"
#include "gui_state.h"
#include "remove_target.h"
#include "source_refresh.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Copy/scan workers and the synchronized preview request/result channel. */

static void *gui_preview_loop(void *data);
static void *gui_job_run(void *data);
static void gui_collect_selection(Gui *ui);

void gui_request_preview(Gui *ui, int reset)
{
    Entry *e = gui_selected(ui->pane + 1);
    if (e && !reset && ui->preview_path && !strcmp(ui->preview_path, e->path) &&
        ui->preview_version == e->version)
        return;
    free(ui->preview_path);
    ui->preview_path = xstrdup(e ? e->path : "");
    ui->preview_version = e ? e->version : 0;
    if (reset) {
        ui->page = 0;
        ui->occurrence_page = 0;
        ui->preview_scroll = ui->details_scroll = 0;
        ui->preview_horizontal = ui->details_horizontal = 0;
    }
    ui->help = 0;
    free(ui->details);
    ui->details = xstrdup(e ? "Loading details..." : "Select a file in the right pane.");
    gui_request_view(ui, VIEW_DETAILS);
    pthread_mutex_lock(&ui->pm);
    atomic_store(&ui->preview_cancel, 1);
    ui->request_seq++;
    free(ui->preview_file);
    ui->preview_file = NULL;
    if (e && !strcmp(e->kind, "file") && (!ui->history || *e->dest))
        ui->preview_file = xstrdup(ui->history ? e->dest : e->path);
    ui->preview_root = ui->history ? ui->c->dfd : ui->c->sfd;
    ui->preview_page = ui->page;
    ui->preview_hex = ui->hex;
    ui->pending_seq = ui->request_seq;
    pthread_cond_signal(&ui->pc);
    pthread_mutex_unlock(&ui->pm);
    preview_free(&ui->preview);
    ui->preview.text = xstrdup(e ? "Loading preview..." : "Select a file.");
    if (ui->texture != R_INVALID_ID) {
        Rtexture_destroy(ui->texture);
        ui->texture = R_INVALID_ID;
    }
}

static void *gui_preview_loop(void *data)
{
    Gui *ui = data;
    for (;;) {
        pthread_mutex_lock(&ui->pm);
        while (!ui->preview_stop && !ui->pending_seq)
            pthread_cond_wait(&ui->pc, &ui->pm);
        if (ui->preview_stop) {
            pthread_mutex_unlock(&ui->pm);
            break;
        }
        char *file = ui->preview_file;
        ui->preview_file = NULL;
        int root = ui->preview_root, page = ui->preview_page, hx = ui->preview_hex,
            seq = ui->pending_seq;
        ui->pending_seq = 0;
        atomic_store(&ui->preview_cancel, 0);
        pthread_mutex_unlock(&ui->pm);
        Preview p = {0};
        int fd = file && root >= 0 ? open_beneath(root, file, O_RDONLY | O_NONBLOCK, 0) : -1;
        if (fd >= 0) {
            activity_log(ui->c, LOG_DEBUG, "preview.start", file, NULL, "%s preview; page/frame %d",
                         hx ? "Hex" : "Format", page);
            int rc = preview_spawn(fd, page, hx, &p, &ui->preview_cancel);
            if (rc < 0 && !atomic_load(&ui->preview_cancel))
                activity_log(ui->c, LOG_WARNING, "preview.failed", file, NULL, "%s",
                             p.text ? p.text : "No decoder diagnostics");
            else if (!rc)
                activity_log(ui->c, LOG_DEBUG, "preview.ready", file, NULL,
                             "Preview %d x %d; pages=%d", p.width, p.height, p.pages);
            close(fd);
        } else if (file && root >= 0) {
            p.error = 1;
            p.text = fmt("Cannot open file for preview: %s", strerror(errno));
            activity_log(ui->c, LOG_WARNING, "preview.open", file, NULL, "%s", p.text);
        } else
            p.text = xstrdup("See metadata in the right panel; byte preview is unavailable.");
        free(file);
        pthread_mutex_lock(&ui->pm);
        if (seq == ui->request_seq) {
            preview_free(&ui->ready);
            ui->ready = p;
            ui->result_seq = seq;
            ui->ready_available = 1;
        } else
            preview_free(&p);
        pthread_mutex_unlock(&ui->pm);
    }
    return NULL;
}

static void *gui_job_run(void *data)
{
    Gui *ui = data;
    int rc = 0;
    atomic_store(&ui->c->files, 0);
    atomic_store(&ui->c->bytes, 0);
    ui->c->error[0] = 0;
    if (strcmp(ui->job, "scan"))
        atomic_store(&ui->c->scan.phase, SCAN_IDLE);
    activity_log(ui->c, LOG_INFO, "operation.start", ui->arg1, ui->arg2, "%s", ui->job);
    if (!strcmp(ui->job, "scan"))
        rc = scan_source(ui->c, ui->complete);
    else if (!strcmp(ui->job, "refresh"))
        rc = refresh_directory(ui->c, ui->arg1);
    else if (!strcmp(ui->job, "copy")) {
        for (int i = 0; i < ui->selection_count && !atomic_load(&ui->c->cancel); i++) {
            rc = refresh_copy_source(ui->c, ui->selections[i]);
            if (!rc)
                rc = copy_entry(ui->c, ui->selections[i], ui->arg2, ui->complete);
            if (rc)
                break;
        }
    } else if (!strcmp(ui->job, "move"))
        rc = move_target(ui->c, ui->arg1, ui->arg2);
    else if (!strcmp(ui->job, "remove"))
        rc = remove_target_copy(ui->c, ui->arg1);
    else if (!strcmp(ui->job, "mkdir")) {
        rc = mkdir_beneath(ui->c->dfd, ui->arg1);
        if (rc)
            fail(ui->c, "mkdir: %s", strerror(errno));
    } else if (!strcmp(ui->job, "audit"))
        rc = audit_target(ui->c);
    else if (!strcmp(ui->job, "reconcile"))
        rc = reconcile_target(ui->c);
    else if (!strcmp(ui->job, "export"))
        rc = export_archive(ui->c);
    else
        for (int i = 0; i < ui->selection_count; i++)
            if ((rc = mark_entries(ui->c, ui->selections[i], ui->job)))
                break;
    if (!rc && atomic_load(&ui->c->cancel))
        rc = fail(ui->c, "Cancelled; completed items remain saved");
    if (!strcmp(ui->job, "scan")) {
        atomic_store(&ui->c->scan.finished, monotonic_ns());
        if (rc)
            atomic_store(&ui->c->scan.phase, SCAN_STOPPED);
    }
    activity_log(ui->c, rc ? (atomic_load(&ui->c->cancel) ? LOG_WARNING : LOG_ERROR) : LOG_INFO,
                 "operation.finish", ui->arg1, ui->arg2, "%s: %s%s", ui->job,
                 rc ? "incomplete; " : "completed", rc ? ui->c->error : "");
    ui->job_result = rc;
    atomic_store(&ui->finished, 1);
    return NULL;
}

int gui_start_job(Gui *ui, const char *job, const char *a, const char *b, int complete)
{
    if (atomic_load(&ui->busy)) {
        if (gui_lazy_active(ui) && !ui->next_job) {
            ui->next_job = xstrdup(job);
            ui->next_arg1 = xstrdup(a);
            ui->next_arg2 = xstrdup(b);
            ui->next_complete = complete;
            atomic_store(&ui->c->cancel, 1);
            gui_notice(ui, "Stopping folder refresh; your operation will start next.");
            return 0;
        }
        gui_notice(ui, "Operation in progress. Esc cancels it and keeps completed copies.");
        return -1;
    }
    free(ui->job);
    free(ui->arg1);
    free(ui->arg2);
    ui->job = xstrdup(job);
    ui->arg1 = xstrdup(a);
    ui->arg2 = xstrdup(b);
    ui->complete = complete;
    atomic_store(&ui->busy, 1);
    atomic_store(&ui->finished, 0);
    atomic_store(&ui->c->cancel, 0);
    int error = pthread_create(&ui->worker, NULL, gui_job_run, ui);
    if (error) {
        atomic_store(&ui->busy, 0);
        gui_notice(ui, "Cannot start operation worker. See Activity.");
        activity_log(ui->c, LOG_ERROR, "operation.thread", NULL, NULL, "%s", strerror(error));
        return -1;
    }
    gui_notice(ui, !strcmp(job, "refresh")
                       ? "Checking this folder in the background; saved catalog is available."
                       : "Working in the background; you can keep browsing.");
    return 0;
}

int gui_lazy_active(Gui *ui)
{
    return atomic_load(&ui->busy) && ui->job && !strcmp(ui->job, "refresh");
}

int gui_operation_busy(Gui *ui)
{
    return ui->next_job || (atomic_load(&ui->busy) && !gui_lazy_active(ui));
}

void gui_refresh_folder(Gui *ui)
{
    free(ui->lazy_path);
    ui->lazy_path = ui->history || ui->c->readonly ? NULL : xstrdup(ui->pane[1].path);
    if (gui_lazy_active(ui))
        atomic_store(&ui->c->cancel, 1);
}

void gui_cancel_job(Gui *ui)
{
    free(ui->next_job);
    free(ui->next_arg1);
    free(ui->next_arg2);
    free(ui->lazy_path);
    ui->next_job = ui->next_arg1 = ui->next_arg2 = ui->lazy_path = NULL;
    if (atomic_load(&ui->busy))
        atomic_store(&ui->c->cancel, 1);
}

void gui_jobs_tick(Gui *ui)
{
    if (atomic_load(&ui->busy))
        return;
    if (ui->next_job) {
        char *job = ui->next_job, *a = ui->next_arg1, *b = ui->next_arg2;
        ui->next_job = ui->next_arg1 = ui->next_arg2 = NULL;
        gui_start_job(ui, job, a, b, ui->next_complete);
        free(job);
        free(a);
        free(b);
    } else if (ui->lazy_path && !ui->history) {
        char *path = ui->lazy_path;
        ui->lazy_path = NULL;
        gui_start_job(ui, "refresh", path, NULL, 0);
        free(path);
    }
}

static void gui_collect_selection(Gui *ui)
{
    for (int i = 0; i < ui->selection_count; i++)
        free(ui->selections[i]);
    free(ui->selections);
    ui->selections = NULL;
    ui->selection_count = 0;
    GuiPane *p = ui->pane + 1;
    for (int i = 0; i < p->mark_count; i++) {
        ui->selections = realloc(ui->selections, (ui->selection_count + 1) * sizeof(char *));
        ui->selections[ui->selection_count++] = xstrdup(p->marks[i]);
    }
    if (!ui->selection_count) {
        Entry *e = gui_selected(p);
        if (e) {
            ui->selections = xmalloc(sizeof(char *));
            ui->selections[ui->selection_count++] = xstrdup(e->path);
        }
    }
}

void gui_copy_selected(Gui *ui, int complete)
{
    if (gui_operation_busy(ui)) {
        gui_notice(ui, "Wait for the operation to finish or press Esc.");
        return;
    }
    if (ui->history) {
        gui_notice(ui, "Press Ctrl+H to switch to the source and copy current files.");
        return;
    }
    gui_collect_selection(ui);
    if (ui->selection_count)
        gui_start_job(ui, "copy", NULL, ui->pane[0].path, complete);
}

void gui_mark_selected(Gui *ui, const char *action)
{
    if (gui_operation_busy(ui)) {
        gui_notice(ui, "Operation in progress; marking is available when it finishes.");
        return;
    }
    if (ui->history) {
        gui_notice(ui, "Make curation decisions in source view (Ctrl+H).");
        return;
    }
    gui_collect_selection(ui);
    if (ui->selection_count)
        gui_start_job(ui, action, NULL, NULL, 0);
}

void gui_jobs_start_preview(Gui *ui)
{
    pthread_create(&ui->preview_thread, NULL, gui_preview_loop, ui);
}

void gui_jobs_stop_preview(Gui *ui)
{
    pthread_mutex_lock(&ui->pm);
    ui->preview_stop = 1;
    atomic_store(&ui->preview_cancel, 1);
    pthread_cond_signal(&ui->pc);
    pthread_mutex_unlock(&ui->pm);
    pthread_join(ui->preview_thread, NULL);
    free(ui->preview_file);
}
