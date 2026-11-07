#include "source_access.h"
#include <dirent.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    dev_t dev;
    ino_t ino;
} Writer;
struct SourceWriters {
    Writer *files;
    size_t count, capacity;
    int64_t refreshed;
};
static int writer_order(const void *left, const void *right)
{
    const Writer *a = left, *b = right;
    if (a->dev != b->dev)
        return a->dev < b->dev ? -1 : 1;
    return (a->ino > b->ino) - (a->ino < b->ino);
}
void source_writers_reset(Catalog *c)
{
    if (c->writers) {
        free(c->writers->files);
        free(c->writers);
        c->writers = NULL;
    }
}
static void refresh_writers(Catalog *c)
{
    struct SourceWriters *w = c->writers;
    int64_t now = monotonic_ns();
    if (w && now - w->refreshed < 30000000000LL)
        return;
    if (!w)
        w = c->writers = xmalloc(sizeof(*w));
    w->refreshed = now;
    w->count = 0;
    DIR *processes = opendir("/proc");
    if (!processes)
        return;
    struct dirent *process;
    while ((process = readdir(processes)) && !atomic_load(&c->cancel)) {
        char *end = NULL;
        long pid = strtol(process->d_name, &end, 10);
        if (pid <= 0 || *end || pid == (long)getpid())
            continue;
        char directory[96];
        snprintf(directory, sizeof(directory), "/proc/%ld/fdinfo", pid);
        DIR *descriptors = opendir(directory);
        if (!descriptors)
            continue;
        struct dirent *entry;
        while ((entry = readdir(descriptors)) && !atomic_load(&c->cancel)) {
            long descriptor = strtol(entry->d_name, &end, 10);
            if (descriptor < 0 || end == entry->d_name || *end)
                continue;
            char name[160], line[512];
            snprintf(name, sizeof(name), "%s/%ld", directory, descriptor);
            FILE *info = fopen(name, "r");
            if (!info)
                continue;
            unsigned long flags = 0;
            while (fgets(line, sizeof(line), info))
                if (sscanf(line, "flags:\t%lo", &flags) == 1)
                    break;
            fclose(info);
            if ((flags & O_ACCMODE) != O_WRONLY && (flags & O_ACCMODE) != O_RDWR)
                continue;
            snprintf(name, sizeof(name), "/proc/%ld/fd/%ld", pid, descriptor);
            struct stat st;
            if (stat(name, &st) || !S_ISREG(st.st_mode))
                continue;
            if (w->count == w->capacity) {
                w->capacity = w->capacity ? w->capacity * 2 : 256;
                Writer *next = realloc(w->files, w->capacity * sizeof(*next));
                if (!next)
                    abort();
                w->files = next;
            }
            w->files[w->count++] = (Writer){st.st_dev, st.st_ino};
        }
        closedir(descriptors);
    }
    closedir(processes);
    if (w->count)
        qsort(w->files, w->count, sizeof(*w->files), writer_order);
}
int source_writer_busy(Catalog *c, const struct stat *st, int fd)
{
    refresh_writers(c);
    Writer key = {st->st_dev, st->st_ino};
    if (c->writers->count &&
        bsearch(&key, c->writers->files, c->writers->count, sizeof(key), writer_order))
        return 1;
    if (fd >= 0) {
        struct flock lock = {.l_type = F_RDLCK, .l_whence = SEEK_SET};
        if (!fcntl(fd, F_GETLK, &lock) && lock.l_type != F_UNLCK)
            return 1;
    }
    return 0;
}
