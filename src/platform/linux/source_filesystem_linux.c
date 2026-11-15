#include "source_filesystem.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/vfs.h>
#include <sys/wait.h>
#include <unistd.h>

/* Linux 6.8 UAPI, also usable with older build headers. Older kernels return
 * ENOTTY. Do not substitute f_fsid: it is not a persistent UUID on every FS. */
#ifndef FS_IOC_GETFSUUID
struct fsuuid2 {
    unsigned char len, uuid[16];
};
#define FS_IOC_GETFSUUID _IOR(0x15, 0, struct fsuuid2)
#endif
#define CC_ZFS_MAGIC 0x2fc12fc1
extern char **environ;

static void unescape_mount(char *s)
{
    char *out = s;
    while (*s) {
        if (s[0] == '\\' && s[1] >= '0' && s[1] <= '7' && s[2] >= '0' && s[2] <= '7' &&
            s[3] >= '0' && s[3] <= '7') {
            *out++ = (char)((s[1] - '0') * 64 + (s[2] - '0') * 8 + s[3] - '0');
            s += 4;
        } else
            *out++ = *s++;
    }
    *out = 0;
}

static char *mount_source(int fd)
{
    char filename[80], *line = NULL;
    size_t capacity = 0;
    unsigned long long mount = 0;
    snprintf(filename, sizeof(filename), "/proc/self/fdinfo/%d", fd);
    FILE *f = fopen(filename, "re");
    if (!f)
        return NULL;
    while (getline(&line, &capacity, f) >= 0)
        if (sscanf(line, "mnt_id: %llu", &mount) == 1)
            break;
    fclose(f);
    f = mount ? fopen("/proc/self/mountinfo", "re") : NULL;
    char *result = NULL;
    if (f) {
        while (getline(&line, &capacity, f) >= 0) {
            unsigned long long id;
            if (sscanf(line, "%llu", &id) != 1 || id != mount)
                continue;
            char *separator = strstr(line, " - ");
            char *save = NULL;
            if (!separator || !strtok_r(separator + 3, " ", &save))
                break;
            char *source = strtok_r(NULL, " \n", &save);
            if (source) {
                unescape_mount(source);
                result = xstrdup(source);
            }
            break;
        }
        fclose(f);
    }
    free(line);
    return result;
}

/* No shell, no source-supplied executables, bounded output and wall time. */
static int zfs_guid(const char *program, const char *object, char guid[21])
{
    const char *dirs[] = {"/usr/sbin", "/sbin", "/usr/bin", "/bin"};
    char binary[128];
    int found = 0;
    for (unsigned i = 0; i < sizeof(dirs) / sizeof(*dirs); i++) {
        snprintf(binary, sizeof(binary), "%s/%s", dirs[i], program);
        if (!access(binary, X_OK)) {
            found = 1;
            break;
        }
    }
    if (!found || !object || !isalnum((unsigned char)*object))
        return -1;
    int pipefd[2];
    if (pipe2(pipefd, O_CLOEXEC))
        return -1;
    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions)) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }
    int setup = posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDOUT_FILENO);
    setup |= posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    setup |= posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    char *argv[] = {binary, "get", "-H", "-p", "-o", "value", "guid", (char *)object, NULL};
    pid_t pid = -1;
    int rc = setup ? setup : posix_spawn(&pid, binary, &actions, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    close(pipefd[1]);
    if (rc) {
        close(pipefd[0]);
        return -1;
    }
    fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
    char output[128];
    size_t used = 0;
    int status = 0, finished = 0;
    int64_t deadline = monotonic_ns() + 5000000000LL;
    while (monotonic_ns() < deadline) {
        ssize_t n = read(pipefd[0], output + used, sizeof(output) - 1 - used);
        if (n > 0)
            used += (size_t)n;
        if (used == sizeof(output) - 1)
            break;
        pid_t done = waitpid(pid, &status, WNOHANG);
        if (done == pid) {
            finished = 1;
            /* Drain the final short line after process exit. */
            n = read(pipefd[0], output + used, sizeof(output) - 1 - used);
            if (n > 0)
                used += (size_t)n;
            break;
        }
        if (done < 0 && errno != EINTR)
            break;
        struct pollfd p = {.fd = pipefd[0], .events = POLLIN};
        poll(&p, 1, 20);
    }
    close(pipefd[0]);
    if (!finished) {
        kill(pid, SIGKILL);
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
        return -1;
    }
    output[used] = 0;
    while (used && isspace((unsigned char)output[used - 1]))
        output[--used] = 0;
    if (!WIFEXITED(status) || WEXITSTATUS(status) || !used || used > 20)
        return -1;
    for (size_t i = 0; i < used; i++)
        if (!isdigit((unsigned char)output[i]))
            return -1;
    errno = 0;
    unsigned long long value = strtoull(output, NULL, 10);
    if (errno || !value)
        return -1;
    snprintf(guid, 21, "%llu", value);
    return 0;
}

int source_filesystem_probe(int parent, const char *leaf, const struct stat *st, char **identity,
                            char **name)
{
    struct stat ps;
    /* Parent descriptors are already safe, open and readable. Never open a
     * device/FIFO just to identify it. A child mount needs its own descriptor. */
    int fd = !fstat(parent, &ps) && ps.st_dev == st->st_dev ? dup(parent) : -1;
    if (fd < 0 && leaf && (S_ISREG(st->st_mode) || S_ISDIR(st->st_mode)))
        fd = openat(parent, leaf, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return -1;
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    if (fstat(fd, &ps) || ps.st_dev != st->st_dev) {
        close(fd);
        return -1;
    }
    *name = mount_source(fd);
    struct statfs fs;
    if (!fstatfs(fd, &fs) && (unsigned long)fs.f_type == CC_ZFS_MAGIC) {
        if (*name) {
            char *pool = xstrdup(*name), pool_guid[21], dataset_guid[21];
            pool[strcspn(pool, "/@")] = 0;
            if (!zfs_guid("zpool", pool, pool_guid) && !zfs_guid("zfs", *name, dataset_guid))
                *identity = fmt("zfs:%s:%s", pool_guid, dataset_guid);
            free(pool);
        }
        return fd;
    }
    struct fsuuid2 uuid = {0};
    if (!ioctl(fd, FS_IOC_GETFSUUID, &uuid) && uuid.len && uuid.len <= sizeof(uuid.uuid)) {
        char hex[33];
        unsigned nonzero = 0;
        for (unsigned i = 0; i < uuid.len; i++) {
            snprintf(hex + i * 2, 3, "%02x", uuid.uuid[i]);
            nonzero |= uuid.uuid[i];
        }
        if (nonzero)
            *identity = fmt("uuid:%s", hex);
    }
    if (!*identity) {
        /* Older kernels: use the filesystem UUID exposed by udev. */
        DIR *d = opendir("/dev/disk/by-uuid");
        struct dirent *de;
        while (d && (de = readdir(d))) {
            struct stat block;
            if (de->d_name[0] == '.' || fstatat(dirfd(d), de->d_name, &block, 0) ||
                !S_ISBLK(block.st_mode) || block.st_rdev != st->st_dev)
                continue;
            char compact[129];
            size_t n = 0;
            for (const char *p = de->d_name; *p && n < sizeof(compact) - 1; p++)
                if (*p != '-')
                    compact[n++] = (char)tolower((unsigned char)*p);
            compact[n] = 0;
            *identity = fmt("uuid:%s", compact);
            break;
        }
        if (d)
            closedir(d);
    }
    return fd;
}
