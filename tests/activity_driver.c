/* Bounded in-memory history, queue draining, filters and append persistence. */
#include "curator.h"
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>
static int inspect(Catalog *c)
{
    for (int attempt = 0; attempt < 1000; attempt++) {
        int count = 0;
        uint64_t revision = 0;
        char error[256];
        ActivityLine *lines = activity_snapshot(c->log, -1, &count, &revision, error);
        int ready = count == 2000 && strstr(lines[count - 1].text, "record 4004");
        activity_lines_free(lines, count);
        if (*error) {
            fprintf(stderr, "%s\n", error);
            return 1;
        }
        if (ready) {
            for (int level = 0; level < 4; level++) {
                lines = activity_snapshot(c->log, level, &count, &revision, error);
                if (count != 500) {
                    activity_lines_free(lines, count);
                    return 2;
                }
                for (int i = 0; i < count; i++)
                    if (lines[i].level != level)
                        return 3;
                activity_lines_free(lines, count);
            }
            return 0;
        }
        struct timespec delay = {.tv_nsec = 10000000};
        nanosleep(&delay, NULL);
    }
    return 4;
}
int main(int argc, char **argv)
{
    if (argc < 2 || argc > 3)
        return 10;
    int fd = open(argv[1], O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0)
        return 11;
    Catalog c = {0};
    c.log = activity_open(fd);
    if (!c.log)
        return 12;
    if (argc == 3) {
        struct rlimit original, zero = {0, 0};
        getrlimit(RLIMIT_FSIZE, &original);
        zero.rlim_max = original.rlim_max;
        signal(SIGXFSZ, SIG_IGN);
        if (setrlimit(RLIMIT_FSIZE, &zero))
            return 14;
        activity_log(&c, LOG_ERROR, "test.failure", NULL, NULL, "Test disk write error");
        char error[256];
        uint64_t revision = 0;
        for (int i = 0; i < 1000 && !revision; i++) {
            struct timespec delay = {.tv_nsec = 10000000};
            nanosleep(&delay, NULL);
            revision = activity_status(c.log, error);
        }
        setrlimit(RLIMIT_FSIZE, &original);
        int failed = !revision || !strstr(error, "Activity file write failed");
        if (!failed)
            puts(error);
        activity_close(c.log);
        close(fd);
        return failed;
    }
    for (int i = 0; i < 4005; i++)
        activity_log(&c, i % 4, "test.record", "file\nwith\tcontrols", NULL, "record %d", i);
    int result = inspect(&c);
    activity_close(c.log);
    if (result)
        return result;
    c.log = activity_open(fd);
    if (!c.log)
        return 13;
    result = inspect(&c);
    activity_close(c.log);
    close(fd);
    return result;
}
