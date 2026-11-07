#ifndef CCURATE_DIAGNOSTICS_WINDOWS_H
#define CCURATE_DIAGNOSTICS_WINDOWS_H

/* Used only by the interactive Windows process, never by restricted workers. */
void cc_win_diagnostics_start(int debug, int graphical);
void cc_win_startup_note(const char *message);
void cc_win_startup_ready(void);
void cc_win_diagnostics_finish(int status);

#endif
