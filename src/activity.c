/* Append-only activity history, independent of SQLite. Disk IO stays on one
 * worker; a bounded recent-record ring serves the GUI without file reads. */
#include "curator.h"
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#define ACTIVITY_RECENT 2000
#define ACTIVITY_QUEUE 2048
struct LogNode {
    char *line;
    int level;
    struct LogNode *next;
};
struct ActivityLog {
    int fd, readfd, stop, count, start, queued;
    uint64_t revision;
    pthread_t worker;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    struct LogNode *first, *last;
    ActivityLine recent[ACTIVITY_RECENT];
    char error[256];
};
static const char *level_names[] = {"DEBUG", "INFO", "WARN", "ERROR"};
static void retain(ActivityLog *log, int level, char *line)
{
    pthread_mutex_lock(&log->mutex);
    int at = (log->start + log->count) % ACTIVITY_RECENT;
    if (log->count == ACTIVITY_RECENT) {
        free(log->recent[at].text);
        log->start = (log->start + 1) % ACTIVITY_RECENT;
    } else
        log->count++;
    log->recent[at] = (ActivityLine){.level = level, .text = line};
    log->revision++;
    pthread_mutex_unlock(&log->mutex);
}
static void load_tail(ActivityLog *log)
{
    if (log->readfd < 0)
        return;
    FILE *f = fdopen(log->readfd, "r");
    if (!f) {
        close(log->readfd);
        return;
    }
    struct stat st;
    if (!fstat(fileno(f), &st) && st.st_size > 1024 * 1024) {
        fseeko(f, st.st_size - 1024 * 1024, SEEK_SET);
        int ch;
        while ((ch = fgetc(f)) != EOF && ch != '\n') {
        }
    }
    char buffer[65536];
    while (fgets(buffer, sizeof(buffer), f)) {
        if (!strchr(buffer, '\n'))
            continue;
        char *field = strstr(buffer, " | ");
        if (!field)
            continue;
        for (int level = 0; level < 4; level++) {
            size_t n = strlen(level_names[level]);
            if (!strncmp(field + 3, level_names[level], n) && !strncmp(field + 3 + n, " | ", 3)) {
                buffer[strcspn(buffer, "\n")] = 0;
                retain(log, level, xstrdup(buffer));
                break;
            }
        }
    }
    fclose(f);
    log->readfd = -1;
}
static void *log_worker(void *arg)
{
    ActivityLog *log = arg;
    load_tail(log);
    for (;;) {
        pthread_mutex_lock(&log->mutex);
        while (!log->first && !log->stop)
            pthread_cond_wait(&log->cond, &log->mutex);
        struct LogNode *n = log->first;
        if (!n) {
            pthread_mutex_unlock(&log->mutex);
            break;
        }
        log->first = n->next;
        if (!log->first)
            log->last = NULL;
        log->queued--;
        pthread_cond_broadcast(&log->cond);
        pthread_mutex_unlock(&log->mutex);
        size_t len = strlen(n->line);
        if (write_all(log->fd, n->line, len) || write_all(log->fd, "\n", 1)) {
            int err = errno;
            pthread_mutex_lock(&log->mutex);
            snprintf(log->error, sizeof(log->error), "Activity file write failed: %s",
                     strerror(err));
            pthread_mutex_unlock(&log->mutex);
        }
        retain(log, n->level, n->line);
        free(n);
    }
    if (fdatasync(log->fd)) {
        fprintf(stderr, "Activity file sync failed: %s\n", strerror(errno));
    }
    return NULL;
}
ActivityLog *activity_open(int state_fd)
{
    int fd = openat(state_fd, "activity.log",
                    O_WRONLY | O_APPEND | O_CREAT | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK | O_NOCTTY,
                    0600);
    if (fd < 0)
        return NULL;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_nlink != 1) {
        close(fd);
        errno = EINVAL;
        return NULL;
    }
    ActivityLog *log = xmalloc(sizeof(*log));
    log->fd = fd;
    log->readfd = openat(state_fd, "activity.log", O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    pthread_mutex_init(&log->mutex, NULL);
    pthread_cond_init(&log->cond, NULL);
    int rc = pthread_create(&log->worker, NULL, log_worker, log);
    if (rc) {
        close(fd);
        if (log->readfd >= 0)
            close(log->readfd);
        pthread_cond_destroy(&log->cond);
        pthread_mutex_destroy(&log->mutex);
        free(log);
        errno = rc;
        return NULL;
    }
    return log;
}
void activity_log(Catalog *c, int level, const char *event, const char *path, const char *target,
                  const char *format, ...)
{
    if (!c || !c->log)
        return;
    ActivityLog *log = c->log;
    if (level < 0 || level > 3)
        level = LOG_INFO;
    char message[4096];
    va_list ap;
    va_start(ap, format);
    vsnprintf(message, sizeof(message), format, ap);
    va_end(ap);
    char *safe = display_path(message), *msg = json_string(safe);
    free(safe);
    safe = display_path(path ? path : "");
    char *src = json_string(safe);
    free(safe);
    safe = display_path(target ? target : "");
    char *dst = json_string(safe);
    free(safe);
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    struct tm tm;
    gmtime_r(&now.tv_sec, &tm);
    char stamp[40];
    strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%S", &tm);
    struct LogNode *node = xmalloc(sizeof(*node));
    node->level = level;
    node->line = fmt("%s.%03ldZ | %s | %s | path=%s | target=%s | %s", stamp, now.tv_nsec / 1000000,
                     level_names[level], event ? event : "application", src, dst, msg);
    free(msg);
    free(src);
    free(dst);
    /* Only background workers/CLI call this API. Backpressure preserves errors
     * if a disk is slower than producers; the renderer never waits here. */
    pthread_mutex_lock(&log->mutex);
    while (log->queued >= ACTIVITY_QUEUE && !log->stop)
        pthread_cond_wait(&log->cond, &log->mutex);
    if (log->stop) {
        pthread_mutex_unlock(&log->mutex);
        free(node->line);
        free(node);
        return;
    }
    if (log->last)
        log->last->next = node;
    else
        log->first = node;
    log->last = node;
    log->queued++;
    pthread_cond_signal(&log->cond);
    pthread_mutex_unlock(&log->mutex);
}
uint64_t activity_status(ActivityLog *log, char error[256])
{
    if (!log) {
        error[0] = 0;
        return 0;
    }
    pthread_mutex_lock(&log->mutex);
    uint64_t revision = log->revision;
    snprintf(error, 256, "%s", log->error);
    pthread_mutex_unlock(&log->mutex);
    return revision;
}
ActivityLine *activity_snapshot(ActivityLog *log, int level, int *count, uint64_t *revision,
                                char error[256])
{
    *count = 0;
    if (!log) {
        error[0] = 0;
        return NULL;
    }
    pthread_mutex_lock(&log->mutex);
    ActivityLine *lines = xmalloc(log->count * sizeof(*lines));
    for (int i = 0; i < log->count; i++) {
        ActivityLine *r = &log->recent[(log->start + i) % ACTIVITY_RECENT];
        if (level < 0 || r->level == level) {
            lines[*count] = (ActivityLine){.level = r->level, .text = xstrdup(r->text)};
            (*count)++;
        }
    }
    if (revision)
        *revision = log->revision;
    snprintf(error, 256, "%s", log->error);
    pthread_mutex_unlock(&log->mutex);
    return lines;
}
void activity_lines_free(ActivityLine *lines, int count)
{
    for (int i = 0; i < count; i++)
        free(lines[i].text);
    free(lines);
}
void activity_close(ActivityLog *log)
{
    if (!log)
        return;
    pthread_mutex_lock(&log->mutex);
    log->stop = 1;
    pthread_cond_broadcast(&log->cond);
    pthread_mutex_unlock(&log->mutex);
    pthread_join(log->worker, NULL);
    close(log->fd);
    for (int i = 0; i < log->count; i++)
        free(log->recent[(log->start + i) % ACTIVITY_RECENT].text);
    pthread_cond_destroy(&log->cond);
    pthread_mutex_destroy(&log->mutex);
    free(log);
}
