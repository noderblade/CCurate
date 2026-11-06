#ifndef CCURATE_GUI_CONTROLLER_H
#define CCURATE_GUI_CONTROLLER_H

/* Commands, prompts, notices and persistence of view preferences. */
#include "gui_types.h"

void gui_notice(Gui *ui, const char *s);
void gui_prompt(Gui *ui, int mode, const char *title, const char *text);
void gui_close_prompt(Gui *ui);
void gui_command(Gui *ui, const char *s);
void gui_prompt_accept(Gui *ui);
void gui_layout_prompt(Gui *ui);
void gui_save_ui(Gui *ui);

#endif /* CCURATE_GUI_CONTROLLER_H */
