#ifndef CCURATE_DIALOG_WINDOWS_H
#define CCURATE_DIALOG_WINDOWS_H
#include <Raven.h>
/* Caller owns the selected UTF-8 path; NULL means cancel or dialog failure. */
char *cc_win_choose_folder(Rid parent, const char *title, const char *initial);
#endif
