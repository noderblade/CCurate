/* Exercise the Linux provider's mount parsing and real child/output handling.
 * Only OS discovery/spawn boundaries are wrapped; no ZFS installation required. */
#include "source_filesystem.h"
#undef NDEBUG
#include <assert.h>
#include <fcntl.h>
#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <sys/vfs.h>
#include <unistd.h>

static int bad, calls;
int __wrap_fstatfs64(int fd, struct statfs64 *fs);
FILE *__wrap_fopen64(const char *path, const char *mode);
int __wrap_access(const char *path, int mode);
int __wrap_posix_spawn(pid_t *pid, const char *path, const posix_spawn_file_actions_t *actions,
                       const posix_spawnattr_t *attributes, char *const argv[], char *const env[]);
int __real_posix_spawn(pid_t *pid, const char *path, const posix_spawn_file_actions_t *actions,
                       const posix_spawnattr_t *attributes, char *const argv[], char *const env[]);
FILE *__real_fopen64(const char *path, const char *mode);
int __wrap_fstatfs64(int fd, struct statfs64 *fs)
{
    (void)fd;
    memset(fs, 0, sizeof(*fs));
    fs->f_type = 0x2fc12fc1;
    return 0;
}
FILE *__wrap_fopen64(const char *path, const char *mode)
{
    const char *data = NULL;
    if (!strncmp(path, "/proc/self/fdinfo/", 18))
        data = "pos:\t0\nmnt_id:\t123\n";
    else if (!strcmp(path, "/proc/self/mountinfo"))
        data = "123 1 0:53 / /mnt/source rw - zfs tank/photos\\040archive rw\n";
    if (!data)
        return __real_fopen64(path, mode);
    FILE *f = tmpfile();
    assert(f);
    fputs(data, f);
    rewind(f);
    return f;
}
int __wrap_access(const char *path, int mode)
{
    (void)mode;
    assert(!strcmp(path, "/usr/sbin/zpool") || !strcmp(path, "/usr/sbin/zfs"));
    return 0;
}
int __wrap_posix_spawn(pid_t *pid, const char *path, const posix_spawn_file_actions_t *actions,
                       const posix_spawnattr_t *attributes, char *const argv[], char *const env[])
{
    assert(!strcmp(argv[1], "get") && !strcmp(argv[2], "-H") && !strcmp(argv[3], "-p"));
    assert(!strcmp(argv[4], "-o") && !strcmp(argv[5], "value") && !strcmp(argv[6], "guid"));
    int pool = !strcmp(path, "/usr/sbin/zpool");
    assert(!strcmp(argv[7], pool ? "tank" : "tank/photos archive") && !argv[8]);
    calls++;
    char *args[] = {"printf", "%s\n",
                    bad    ? "not-a-guid"
                    : pool ? "1234567890123456789"
                           : "9876543210987654321",
                    NULL};
    return __real_posix_spawn(pid, "/usr/bin/printf", actions, attributes, args, env);
}
int main(void)
{
    int root = open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    struct stat st;
    assert(root >= 0 && !fstat(root, &st));
    char *identity = NULL, *name = NULL;
    int fd = source_filesystem_probe(root, NULL, &st, &identity, &name);
    assert(fd >= 0 && calls == 2);
    assert(!strcmp(identity, "zfs:1234567890123456789:9876543210987654321"));
    assert(!strcmp(name, "tank/photos archive"));
    close(fd);
    free(identity);
    free(name);
    bad = 1;
    identity = name = NULL;
    fd = source_filesystem_probe(root, NULL, &st, &identity, &name);
    assert(fd >= 0 && !identity && calls == 3);
    close(fd);
    close(root);
    free(name);
    puts("PASS ZFS pool+dataset GUID / mount escapes / literal argv / invalid GUID refusal");
    return 0;
}
