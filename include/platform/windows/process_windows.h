#ifndef CCURATE_PROCESS_WINDOWS_H
#define CCURATE_PROCESS_WINDOWS_H
#include "native_windows.h"
typedef struct {
    HANDLE process, job, output, errors;
} CCWinProcess;
int cc_win_spawn(CCWinProcess *child, int input, const char *const *arguments, int restricted);
void cc_win_process_close(CCWinProcess *child, int terminate);
/* Nonblocking: -1 error, 0 no bytes available, positive bytes read. */
int cc_win_pipe_read(HANDLE pipe, void *buffer, size_t capacity, int *ended);
int cc_win_elevated(void);
#endif
