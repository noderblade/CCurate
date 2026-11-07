#include "platform/windows/native_windows.h"
#include "source_access.h"

void source_writers_reset(Catalog *c)
{
    (void)c;
}
int source_writer_busy(Catalog *c, const struct stat *st, int fd)
{
    (void)c;
    (void)st;
    if (fd < 0)
        return 0;
    /* A sharing probe fails if another handle currently permits writing. It
     * neither obtains a write handle nor changes the source's access policy. */
    HANDLE probe =
        ReOpenFile(cc_win_handle(fd), FILE_READ_DATA, FILE_SHARE_READ | FILE_SHARE_DELETE, 0);
    if (probe != INVALID_HANDLE_VALUE) {
        CloseHandle(probe);
        return 0;
    }
    DWORD error = GetLastError();
    return error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION;
}
