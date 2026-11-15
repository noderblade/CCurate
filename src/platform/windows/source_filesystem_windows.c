#include "platform/windows/native_windows.h"
#include "source_filesystem.h"
#include <wctype.h>

int source_filesystem_probe(int parent, const char *leaf, const struct stat *st, char **identity,
                            char **name)
{
    struct stat ps;
    int fd = !fstat(parent, &ps) && ps.st_dev == st->st_dev ? dup(parent) : -1;
    if (fd < 0 && leaf)
        fd = openat(parent, leaf, O_PATH | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0)
        return -1;
    if (fstat(fd, &ps) || ps.st_dev != st->st_dev) {
        close(fd);
        return -1;
    }
    wchar_t path[32768];
    DWORD n = GetFinalPathNameByHandleW(cc_win_handle(fd), path, 32768, VOLUME_NAME_GUID);
    if (n && n < 32768 && !wcsncmp(path, L"\\\\?\\Volume{", 11)) {
        wchar_t *end = wcschr(path, L'}');
        if (end) {
            end[1] = 0;
            for (wchar_t *p = path; *p; p++)
                *p = (wchar_t)towlower(*p);
            char *guid = cc_win_utf8(path + 4);
            if (guid) {
                *identity = fmt("windows:%s", guid);
                *name = guid;
            }
        }
    }
    return fd;
}
