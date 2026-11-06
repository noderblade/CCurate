#ifndef CCURATE_GUI_JOBS_H
#define CCURATE_GUI_JOBS_H

/* Copy/scan workers and the synchronized preview request/result channel. */
#include "gui_types.h"

void gui_request_preview(Gui *ui, int reset);
int gui_start_job(Gui *ui, const char *job, const char *a, const char *b, int complete);
void gui_copy_selected(Gui *ui, int complete);
void gui_mark_selected(Gui *ui, const char *action);
void gui_jobs_start_preview(Gui *ui);
void gui_jobs_stop_preview(Gui *ui);

#endif /* CCURATE_GUI_JOBS_H */
