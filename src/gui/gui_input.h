#ifndef CCURATE_GUI_INPUT_H
#define CCURATE_GUI_INPUT_H

/* Keyboard and pointer dispatch for the main workspace. */
#include "gui_types.h"
#include <Raven.h>

int gui_handle_event(Gui *ui, const REvent *e);

#endif /* CCURATE_GUI_INPUT_H */
