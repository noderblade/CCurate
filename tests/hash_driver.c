/* Deterministically mutate a disposable fixture after the first hash read. No
 * production test hook, timing race, or writer-process discovery is involved. */
#include "curator.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

ssize_t __real_read(int fd, void *buffer, size_t size);
ssize_t __wrap_read(int fd, void *buffer, size_t size);
static int watched = -1, writer = -1, mutation;
static unsigned calls;
ssize_t __wrap_read(int fd, void *buffer, size_t size)
{
    if (fd != watched)
        return __real_read(fd, buffer, size);
    calls++;
    if (mutation == 4) {
        errno = EAGAIN;
        return -1;
    }
    ssize_t result = __real_read(fd, buffer, size);
    if (calls == 1) {
        int rc = 0;
        if (mutation == 1)
            rc = ftruncate(writer, 4 * 1024 * 1024);
        else if (mutation == 2)
            rc = ftruncate(writer, 1);
        else if (mutation == 3) {
            rc = pwrite(writer, "changed", 7, 1024 * 1024) == 7 ? 0 : -1;
            /* Distinct metadata even on filesystems with coarse clock ticks. */
            struct timespec stamp[2] = {{1700000000, 0}, {1700000000, 0}};
            rc |= futimens(writer, stamp);
        }
        if (rc) {
            perror("fixture mutation");
            exit(2);
        }
    }
    return result;
}
int main(void)
{
    for (mutation = 0; mutation <= 4; mutation++) {
        char path[] = "/tmp/ccurate-hash-XXXXXX";
        writer = mkstemp(path);
        if (writer < 0 || ftruncate(writer, 2 * 1024 * 1024))
            return 2;
        watched = open(path, O_RDONLY | O_CLOEXEC);
        unlink(path);
        if (watched < 0)
            return 2;
        Catalog catalog = {0};
        char hash[129] = "must be cleared";
        calls = 0;
        int rc = hash_fd(&catalog, watched, hash, NULL);
        close(watched);
        close(writer);
        if (!mutation) {
            if (rc || strlen(hash) != 128 || calls != 2)
                return 1;
        } else if (!rc || *hash || calls != 1 || strncmp(catalog.error, "UNSCANNED:", 10)) {
            fprintf(stderr, "mutation %d: rc=%d reads=%u error=%s\n", mutation, rc, calls,
                    catalog.error);
            return 1;
        }
    }
    puts("Stable, growing, truncated, rewritten and busy hash reads: PASS");
    return 0;
}
