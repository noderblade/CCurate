#ifndef CCURATE_DESKTOP_OPEN_H
#define CCURATE_DESKTOP_OPEN_H

#include <stddef.h>
#include <sys/types.h>

/* A short-lived launcher, never the external application's process. */
typedef struct {
#ifdef _WIN32
    void *native;
#endif
    pid_t pid;
    int fd;
    size_t used;
    double started;
    char message[4096];
} DesktopOpen;

int desktop_open_start(DesktopOpen *job, int root, const char *relative);
/* 0: pending; 1: launch accepted; -1: failed. message remains available. */
int desktop_open_poll(DesktopOpen *job);
void desktop_open_stop(DesktopOpen *job);
int desktop_open_worker(int root, int result_fd, const char *relative);

#endif
