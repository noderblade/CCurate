#ifndef CCURATE_FS_WINDOWS_H
#define CCURATE_FS_WINDOWS_H

/* Private Windows adapter for the descriptor-based core. Never included by a
 * Linux build or by Raven. Read the CRT declarations before local aliases. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifndef O_CLOEXEC
#define O_CLOEXEC 0x100000
#endif
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0x200000
#endif
#ifndef O_DIRECTORY
#define O_DIRECTORY 0x400000
#endif
#ifndef O_NONBLOCK
#define O_NONBLOCK 0
#endif
#ifndef O_NOATIME
#define O_NOATIME 0
#endif
#ifndef O_NOCTTY
#define O_NOCTTY 0
#endif
#ifndef O_PATH
#define O_PATH 0x800000
#endif
#define AT_SYMLINK_NOFOLLOW 0x100
#define AT_EMPTY_PATH 0x1000
#define AT_REMOVEDIR 0x200
#define LOCK_EX 2
#define LOCK_NB 4
#define RENAME_NOREPLACE 1
#define STATX_BTIME 0x800
#ifndef S_IFLNK
#define S_IFLNK 0120000
#endif
#ifndef S_ISLNK
#define S_ISLNK(m) (((m) & S_IFMT) == S_IFLNK)
#endif
#ifndef S_ISSOCK
#define S_ISSOCK(m) (((m) & S_IFMT) == 0140000)
#endif
#ifndef S_ISBLK
#define S_ISBLK(m) (((m) & S_IFMT) == 0060000)
#endif

struct cc_win_stat {
    uint64_t st_dev, st_ino, st_nlink;
    int64_t st_size, st_uid, st_gid;
    mode_t st_mode;
    struct timespec st_atim, st_mtim, st_ctim, birth;
    uint32_t attributes;
};
struct statx {
    unsigned stx_mask;
    struct {
        int64_t tv_sec;
        uint32_t tv_nsec;
    } stx_btime;
};
struct statvfs {
    uint64_t f_bavail, f_frsize, f_blocks;
};
struct cc_win_dirent {
    char d_name[1025];
};
typedef struct CCWinDir CCWinDir;

int cc_win_open(const char *path, int flags, ...);
int cc_win_openat(int root, const char *leaf, int flags, ...);
int cc_win_stat(const char *path, struct cc_win_stat *st);
int cc_win_fstat(int fd, struct cc_win_stat *st);
int cc_win_fstatat(int root, const char *leaf, struct cc_win_stat *st, int flags);
int cc_win_statx(int root, const char *leaf, int flags, unsigned mask, struct statx *st);
int cc_win_mkdirat(int root, const char *leaf, mode_t mode);
int cc_win_unlinkat(int root, const char *leaf, int flags);
int cc_win_renameat2(int from, const char *a, int to, const char *b, unsigned flags);
int cc_win_renameat(int from, const char *a, int to, const char *b);
int cc_win_fsync(int fd);
int cc_win_fchmod(int fd, mode_t mode);
int cc_win_futimens(int fd, const struct timespec ts[2]);
int cc_win_ftruncate(int fd, int64_t size);
int cc_win_flock(int fd, int operation);
int cc_win_fstatvfs(int fd, struct statvfs *fs);
ssize_t cc_win_pread(int fd, void *buffer, size_t size, int64_t offset);
ssize_t cc_win_getrandom(void *buffer, size_t size, unsigned flags);
ssize_t cc_win_readlinkat(int root, const char *leaf, char *buffer, size_t size);
CCWinDir *cc_win_fdopendir(int fd);
struct cc_win_dirent *cc_win_readdir(CCWinDir *dir);
int cc_win_closedir(CCWinDir *dir);
char *cc_win_realpath(const char *path, char *resolved);
char *cc_win_getcwd(char *buffer, size_t size);
FILE *cc_win_fopen(const char *path, const char *mode);
char *cc_win_strcasestr(const char *haystack, const char *needle);
int cc_win_descendant(const char *path, const char *parent);
char *cc_win_parent_path(const char *path);
char *cc_win_attributes(int fd, char error[256]);
int cc_win_content_supported(int fd, char *error, size_t size);
ssize_t cc_win_getline(char **line, size_t *capacity, FILE *file);
int cc_win_validate_roots(const char *source, const char *destination, char *error, size_t size);
struct tm *cc_win_gmtime_r(const time_t *time, struct tm *out);
struct tm *cc_win_localtime_r(const time_t *time, struct tm *out);

#ifndef CCURATE_WINDOWS_NATIVE
#define stat cc_win_stat
#define fstat cc_win_fstat
#define fstatat cc_win_fstatat
#define statx(...) cc_win_statx(__VA_ARGS__)
#define dev_t uint64_t
#define ino_t uint64_t
#define open cc_win_open
#define openat cc_win_openat
#define mkdirat cc_win_mkdirat
#define unlinkat cc_win_unlinkat
#define renameat2 cc_win_renameat2
#define renameat cc_win_renameat
#define fsync cc_win_fsync
#define fdatasync cc_win_fsync
#define fchmod cc_win_fchmod
#define futimens cc_win_futimens
#define ftruncate cc_win_ftruncate
#define flock cc_win_flock
#define fstatvfs cc_win_fstatvfs
#define pread cc_win_pread
#define getrandom cc_win_getrandom
#define readlinkat cc_win_readlinkat
#define DIR CCWinDir
#define dirent cc_win_dirent
#define fdopendir cc_win_fdopendir
#define readdir cc_win_readdir
#define closedir cc_win_closedir
#define realpath cc_win_realpath
#define getcwd cc_win_getcwd
#define fopen cc_win_fopen
#define strcasestr cc_win_strcasestr
#define lseek _lseeki64
#define gmtime_r cc_win_gmtime_r
#define localtime_r cc_win_localtime_r
#define getline cc_win_getline
#endif
#endif
