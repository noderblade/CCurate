#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* Test-only LD_PRELOAD fixture. Hold the first scan after several committed
 * inventory batches, so GUI checks do not depend on directory enumeration speed.
 * Only fixture names match; the application and its build remain unchanged. */
static atomic_uint seen;
static void hold_fixture(const char *name)
{
    if (strncmp(name, "doc-", 4) || atomic_fetch_add(&seen, 1) != 511)
        return;
    const char *gate = getenv("CCURATE_TEST_ENUM_GATE");
    const char *ready = getenv("CCURATE_TEST_ENUM_READY");
    if (!gate || !ready || access(gate, F_OK))
        return;
    int fd = open(ready, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd >= 0)
        close(fd);
    struct timespec start, now, delay = {0, 1000000};
    clock_gettime(CLOCK_MONOTONIC, &start);
    do {
        nanosleep(&delay, NULL);
        clock_gettime(CLOCK_MONOTONIC, &now);
    } while (!access(gate, F_OK) && now.tv_sec - start.tv_sec < 30);
}

int statx(int dirfd, const char *path, int flags, unsigned mask, struct statx *info)
{
    int (*next)(int, const char *, int, unsigned, struct statx *) = dlsym(RTLD_NEXT, "statx");
    int rc = next(dirfd, path, flags, mask, info);
    int saved_errno = errno;
    if (!rc)
        hold_fixture(path);
    errno = saved_errno;
    return rc;
}
