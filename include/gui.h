#ifndef CCURATE_GUI_H
#define CCURATE_GUI_H

/* Application lifecycle and Raven callback adapters. */
#include "curator.h"

int gui_main(Catalog *c, int historical);
/* Choose roots by browsing. Owns/replaces the strings; returns 1, 0 on cancel,
 * or -1 when the graphical interface cannot be initialized. */
int gui_choose_roots(char **source, char **destination, int historical, const char *error);

#endif /* CCURATE_GUI_H */
