/* Test-only LD_PRELOAD gates for real process interruption and concurrent edits.
 * Never linked into the application. All paths are disposable test fixtures. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void gate(const char *point)
{
    const char *mode = getenv("CCURATE_TEST_REMOVE_POINT");
    const char *wait = getenv("CCURATE_TEST_REMOVE_GATE");
    const char *ready = getenv("CCURATE_TEST_REMOVE_READY");
    if (!mode || strcmp(mode, point) || !wait || !ready)
        return;
    int fd = open(ready, O_CREAT | O_WRONLY | O_TRUNC, 0600);
    if (fd < 0)
        _exit(97);
    close(fd);
    while (!access(wait, F_OK))
        usleep(1000);
}

int renameat2(int from, const char *a, int to, const char *b, unsigned flags)
{
    int (*real_rename)(int, const char *, int, const char *, unsigned) =
        dlsym(RTLD_NEXT, "renameat2");
    int stage = !strncmp(b, ".ccurate-remove-", 16);
    if (stage)
        gate("before-stage");
    int rc = real_rename(from, a, to, b, flags);
    if (!rc && stage)
        gate("staged");
    return rc;
}

int unlinkat(int parent, const char *leaf, int flags)
{
    int (*real_unlink)(int, const char *, int) = dlsym(RTLD_NEXT, "unlinkat");
    int rc = real_unlink(parent, leaf, flags);
    if (!rc && !strncmp(leaf, ".ccurate-remove-", 16))
        gate("unlinked");
    return rc;
}
