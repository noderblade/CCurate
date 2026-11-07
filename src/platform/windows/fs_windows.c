#define CCURATE_WINDOWS_NATIVE
#include "curator.h"
#include "platform/windows/native_windows.h"
#include <aclapi.h>
#include <bcrypt.h>
#include <limits.h>
#include <sddl.h>
#include <stdarg.h>
#include <winioctl.h>
#include <winternl.h>

_Static_assert(sizeof(off_t) >= 8, "Windows builds require 64-bit file offsets");

#ifndef FILE_OPEN
#define FILE_OPEN 1
#define FILE_CREATE 2
#define FILE_OPEN_IF 3
#define FILE_OVERWRITE_IF 5
#endif
#ifndef FILE_DIRECTORY_FILE
#define FILE_DIRECTORY_FILE 0x00000001
#endif
#ifndef FILE_SYNCHRONOUS_IO_NONALERT
#define FILE_SYNCHRONOUS_IO_NONALERT 0x00000020
#endif
#ifndef FILE_OPEN_REPARSE_POINT
#define FILE_OPEN_REPARSE_POINT 0x00200000
#endif
#ifndef OBJ_DONT_REPARSE
#define OBJ_DONT_REPARSE 0x1000
#endif

typedef NTSTATUS(NTAPI *CreateFileFn)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK,
                                      PLARGE_INTEGER, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
typedef ULONG(WINAPI *StatusErrorFn)(NTSTATUS);
typedef NTSTATUS(NTAPI *SetFileFn)(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, FILE_INFORMATION_CLASS);
static CreateFileFn nt_create;
static StatusErrorFn nt_error;
static SetFileFn nt_set;
static INIT_ONCE nt_once = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK load_nt(PINIT_ONCE once, PVOID parameter, PVOID *context)
{
    (void)once;
    (void)parameter;
    (void)context;
    HMODULE module = GetModuleHandleW(L"ntdll.dll");
    nt_create = (CreateFileFn)(void *)GetProcAddress(module, "NtCreateFile");
    nt_error = (StatusErrorFn)(void *)GetProcAddress(module, "RtlNtStatusToDosError");
    nt_set = (SetFileFn)(void *)GetProcAddress(module, "NtSetInformationFile");
    return nt_create && nt_error && nt_set;
}

int cc_win_errno(DWORD error)
{
    switch (error) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
        errno = ENOENT;
        break;
    case ERROR_ALREADY_EXISTS:
    case ERROR_FILE_EXISTS:
        errno = EEXIST;
        break;
    case ERROR_ACCESS_DENIED:
        errno = EACCES;
        break;
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:
        errno = EBUSY;
        break;
    case ERROR_DIRECTORY:
        errno = ENOTDIR;
        break;
    case ERROR_NOT_SAME_DEVICE:
        errno = EXDEV;
        break;
    case ERROR_DISK_FULL:
    case ERROR_HANDLE_DISK_FULL:
        errno = ENOSPC;
        break;
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY:
        errno = ENOMEM;
        break;
    case ERROR_INVALID_HANDLE:
        errno = EBADF;
        break;
    case ERROR_NOT_SUPPORTED:
    case ERROR_INVALID_FUNCTION:
        errno = ENOTSUP;
        break;
    case ERROR_INVALID_NAME:
    case ERROR_INVALID_PARAMETER:
        errno = EINVAL;
        break;
    case ERROR_DIR_NOT_EMPTY:
        errno = ENOTEMPTY;
        break;
    case ERROR_CANT_ACCESS_FILE:
        errno = ELOOP;
        break;
    default:
        errno = EIO;
        break;
    }
    return -1;
}
wchar_t *cc_win_wide(const char *text)
{
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, NULL, 0);
    if (!n) {
        errno = EILSEQ;
        return NULL;
    }
    wchar_t *out = xmalloc((size_t)n * sizeof(*out));
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, out, n)) {
        free(out);
        errno = EILSEQ;
        return NULL;
    }
    return out;
}
char *cc_win_utf8(const wchar_t *text)
{
    int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1, NULL, 0, NULL, NULL);
    if (!n) {
        errno = EILSEQ;
        return NULL;
    }
    char *out = xmalloc((size_t)n);
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1, out, n, NULL, NULL)) {
        free(out);
        errno = EILSEQ;
        return NULL;
    }
    return out;
}
HANDLE cc_win_handle(int fd)
{
    return (HANDLE)_get_osfhandle(fd);
}
int cc_win_fd(HANDLE h, int flags)
{
    if (h == INVALID_HANDLE_VALUE)
        return cc_win_errno(GetLastError());
    int fd = _open_osfhandle((intptr_t)h, (flags & (O_RDONLY | O_WRONLY | O_RDWR | O_APPEND)) |
                                              _O_BINARY | _O_NOINHERIT);
    if (fd < 0)
        CloseHandle(h);
    return fd;
}
wchar_t *cc_win_handle_path(HANDLE h)
{
    DWORD n = GetFinalPathNameByHandleW(h, NULL, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!n) {
        cc_win_errno(GetLastError());
        return NULL;
    }
    wchar_t *path = xmalloc(((size_t)n + 1) * sizeof(*path));
    DWORD got = GetFinalPathNameByHandleW(h, path, n + 1, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!got || got > n) {
        free(path);
        cc_win_errno(GetLastError());
        return NULL;
    }
    return path;
}
static int component(const char *name, int create)
{
    if (!*name || !strcmp(name, "..") || strpbrk(name, "/\\:"))
        return 0;
    if (!create)
        return 1;
    size_t n = strlen(name);
    if (name[n - 1] == '.' || name[n - 1] == ' ' || strpbrk(name, "<>\"|?*"))
        return 0;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++)
        if (*p < 32)
            return 0;
    char device[16] = {0};
    size_t k = strcspn(name, ".");
    if (k < sizeof(device)) {
        memcpy(device, name, k);
        if (!_stricmp(device, "CON") || !_stricmp(device, "PRN") || !_stricmp(device, "AUX") ||
            !_stricmp(device, "NUL"))
            return 0;
        if (k == 4 && (!_strnicmp(device, "COM", 3) || !_strnicmp(device, "LPT", 3)) &&
            device[3] >= '1' && device[3] <= '9')
            return 0;
    }
    return 1;
}
static int ordinary_directory(HANDLE handle)
{
    DWORD flags = 0;
    if (!GetFileInformationByHandleEx(handle, FileCaseSensitiveInfo, &flags, sizeof(flags))) {
        DWORD error = GetLastError();
        if (error != ERROR_INVALID_PARAMETER && error != ERROR_NOT_SUPPORTED &&
            error != ERROR_INVALID_FUNCTION) {
            cc_win_errno(error);
            return 0;
        }
    } else if (flags & 1) {
        /* Case-sensitive NTFS directories need different lookup/collision rules.
         * Refuse them instead of hashing the wrong case variant of a name. */
        errno = ENOTSUP;
        return 0;
    }
    return 1;
}
HANDLE cc_win_relative_handle(int root, const char *leaf, ACCESS_MASK access, ULONG disposition,
                              ULONG options, int allow_reparse)
{
    if (!component(leaf, disposition != FILE_OPEN)) {
        errno = EINVAL;
        return INVALID_HANDLE_VALUE;
    }
    HANDLE parent = cc_win_handle(root);
    FILE_ATTRIBUTE_TAG_INFO tag;
    if (!GetFileInformationByHandleEx(parent, FileAttributeTagInfo, &tag, sizeof(tag))) {
        cc_win_errno(GetLastError());
        return INVALID_HANDLE_VALUE;
    }
    if (!(tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
        (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        errno = ELOOP;
        return INVALID_HANDLE_VALUE;
    }
    if (!ordinary_directory(parent))
        return INVALID_HANDLE_VALUE;
    if (!InitOnceExecuteOnce(&nt_once, load_nt, NULL, NULL)) {
        errno = ENOTSUP;
        return INVALID_HANDLE_VALUE;
    }
    /* An empty native name reopens the anchored directory itself. Supplying
     * FILE_DIRECTORY_FILE avoids ReOpenFile's file-oriented flag translation. */
    wchar_t *wide = cc_win_wide(!strcmp(leaf, ".") ? "" : leaf);
    if (!wide)
        return INVALID_HANDLE_VALUE;
    size_t bytes = wcslen(wide) * sizeof(*wide);
    if (bytes > USHRT_MAX - sizeof(*wide)) {
        free(wide);
        errno = ENAMETOOLONG;
        return INVALID_HANDLE_VALUE;
    }
    UNICODE_STRING name = {
        .Length = (USHORT)bytes, .MaximumLength = (USHORT)(bytes + sizeof(*wide)), .Buffer = wide};
    OBJECT_ATTRIBUTES object = {.Length = sizeof(object),
                                .RootDirectory = parent,
                                .ObjectName = &name,
                                .Attributes = OBJ_CASE_INSENSITIVE | OBJ_DONT_REPARSE};
    IO_STATUS_BLOCK status;
    HANDLE h = INVALID_HANDLE_VALUE;
    NTSTATUS result = nt_create(
        &h, access | SYNCHRONIZE, &object, &status, NULL, FILE_ATTRIBUTE_NORMAL,
        (options & FILE_DIRECTORY_FILE) ? FILE_SHARE_READ | FILE_SHARE_WRITE
                                        : FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        disposition, options | FILE_OPEN_REPARSE_POINT | FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0);
    free(wide);
    if (result < 0) {
        DWORD error = nt_error(result);
        if (getenv("CCURATE_WINDOWS_DIAGNOSTICS"))
            fprintf(stderr,
                    "Windows relative open: %s, access 0x%lx, options 0x%lx: "
                    "NTSTATUS 0x%lx, error %lu\n",
                    leaf, (unsigned long)access, (unsigned long)options, (unsigned long)result,
                    (unsigned long)error);
        cc_win_errno(error);
        return INVALID_HANDLE_VALUE;
    }
    if (!GetFileInformationByHandleEx(h, FileAttributeTagInfo, &tag, sizeof(tag))) {
        DWORD error = GetLastError();
        CloseHandle(h);
        cc_win_errno(error);
        return INVALID_HANDLE_VALUE;
    }
    if (!allow_reparse && (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        CloseHandle(h);
        errno = ELOOP;
        return INVALID_HANDLE_VALUE;
    }
    return h;
}
static wchar_t *absolute_path(const char *path)
{
    wchar_t *input = cc_win_wide(path);
    if (!input)
        return NULL;
    for (wchar_t *p = input; *p; p++)
        if (*p == '/')
            *p = '\\';
    if (!wcsncmp(input, L"\\\\?\\", 4))
        return input;
    DWORD n = GetFullPathNameW(input, 0, NULL, NULL);
    if (!n) {
        free(input);
        cc_win_errno(GetLastError());
        return NULL;
    }
    wchar_t *full = xmalloc(((size_t)n + 1) * sizeof(*full));
    DWORD got = GetFullPathNameW(input, n + 1, full, NULL);
    free(input);
    if (!got || got > n) {
        free(full);
        errno = EINVAL;
        return NULL;
    }
    size_t length = wcslen(full);
    wchar_t *extended = xmalloc((length + 9) * sizeof(*extended));
    if (!wcsncmp(full, L"\\\\", 2)) {
        wcscpy(extended, L"\\\\?\\UNC\\");
        wcscat(extended, full + 2);
    } else {
        wcscpy(extended, L"\\\\?\\");
        wcscat(extended, full);
    }
    free(full);
    return extended;
}
static DWORD file_access(int flags)
{
    if (flags & O_PATH)
        return FILE_READ_ATTRIBUTES;
    /* A directory anchor does not need enumeration/data access. Keeping that
     * right off the anchor avoids an NTFS rename-target sharing conflict. */
    if (flags & O_DIRECTORY)
        return FILE_READ_ATTRIBUTES | FILE_TRAVERSE | READ_CONTROL;
    return ((flags & O_RDWR)     ? GENERIC_READ | GENERIC_WRITE
            : (flags & O_WRONLY) ? GENERIC_WRITE
                                 : GENERIC_READ) |
           READ_CONTROL | FILE_READ_ATTRIBUTES;
}
int cc_win_open(const char *path, int flags, ...)
{
    wchar_t *wide = absolute_path(path);
    if (!wide)
        return -1;
    DWORD disposition = flags & O_CREAT   ? (flags & O_EXCL    ? CREATE_NEW
                                             : flags & O_TRUNC ? CREATE_ALWAYS
                                                               : OPEN_ALWAYS)
                        : flags & O_TRUNC ? TRUNCATE_EXISTING
                                          : OPEN_EXISTING;
    HANDLE h = CreateFileW(
        wide, file_access(flags),
        (flags & O_DIRECTORY) ? FILE_SHARE_READ | FILE_SHARE_WRITE
                              : FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL, disposition,
        FILE_FLAG_BACKUP_SEMANTICS | ((flags & O_NOFOLLOW) ? FILE_FLAG_OPEN_REPARSE_POINT : 0),
        NULL);
    free(wide);
    if (h == INVALID_HANDLE_VALUE)
        return cc_win_errno(GetLastError());
    FILE_ATTRIBUTE_TAG_INFO tag;
    if (!GetFileInformationByHandleEx(h, FileAttributeTagInfo, &tag, sizeof(tag))) {
        DWORD error = GetLastError();
        CloseHandle(h);
        return cc_win_errno(error);
    }
    if (((flags & O_DIRECTORY) && !(tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY)) ||
        ((flags & O_NOFOLLOW) && (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))) {
        CloseHandle(h);
        errno = (flags & O_NOFOLLOW) ? ELOOP : ENOTDIR;
        return -1;
    }
    return cc_win_fd(h, flags);
}
int cc_win_openat(int root, const char *leaf, int flags, ...)
{
    ULONG disposition = flags & O_CREAT ? (flags & O_EXCL    ? FILE_CREATE
                                           : flags & O_TRUNC ? FILE_OVERWRITE_IF
                                                             : FILE_OPEN_IF)
                                        : FILE_OPEN;
    HANDLE h = cc_win_relative_handle(root, leaf, file_access(flags), disposition,
                                      flags & O_DIRECTORY ? FILE_DIRECTORY_FILE : 0, 0);
    return h == INVALID_HANDLE_VALUE ? -1 : cc_win_fd(h, flags);
}
static struct timespec timestamp(LONGLONG ticks)
{
    int64_t relative = ticks - 116444736000000000LL;
    struct timespec result = {(time_t)(relative / 10000000), (long)((relative % 10000000) * 100)};
    if (result.tv_nsec < 0) {
        result.tv_sec--;
        result.tv_nsec += 1000000000;
    }
    return result;
}
static int handle_stat(HANDLE h, struct cc_win_stat *st)
{
    BY_HANDLE_FILE_INFORMATION id;
    FILE_BASIC_INFO info;
    FILE_STANDARD_INFO sizes;
    const char *operation = NULL;
    if (!GetFileInformationByHandle(h, &id))
        operation = "GetFileInformationByHandle";
    else if (!GetFileInformationByHandleEx(h, FileBasicInfo, &info, sizeof(info)))
        operation = "FileBasicInfo";
    else if (!GetFileInformationByHandleEx(h, FileStandardInfo, &sizes, sizeof(sizes)))
        operation = "FileStandardInfo";
    if (operation) {
        DWORD error = GetLastError();
        if (getenv("CCURATE_WINDOWS_DIAGNOSTICS"))
            fprintf(stderr, "Windows metadata: %s failed: error %lu, handle %p\n", operation,
                    (unsigned long)error, (void *)h);
        return cc_win_errno(error);
    }
    memset(st, 0, sizeof(*st));
    st->st_dev = id.dwVolumeSerialNumber;
    st->st_ino = ((uint64_t)id.nFileIndexHigh << 32) | id.nFileIndexLow;
    st->st_nlink = id.nNumberOfLinks;
    st->st_size = sizes.EndOfFile.QuadPart;
    st->st_uid = st->st_gid = -1;
    st->attributes = info.FileAttributes;
    st->st_mode = (info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ? S_IFLNK | 0777
                  : (info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY)   ? S_IFDIR | 0755
                                                                       : S_IFREG | 0644;
    if (info.FileAttributes & FILE_ATTRIBUTE_READONLY)
        st->st_mode &= ~0222;
    st->st_atim = timestamp(info.LastAccessTime.QuadPart);
    st->st_mtim = timestamp(info.LastWriteTime.QuadPart);
    st->st_ctim = timestamp(info.ChangeTime.QuadPart);
    st->birth = timestamp(info.CreationTime.QuadPart);
    return 0;
}
int cc_win_fstat(int fd, struct cc_win_stat *st)
{
    return handle_stat(cc_win_handle(fd), st);
}
int cc_win_stat(const char *path, struct cc_win_stat *st)
{
    int fd = cc_win_open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    int rc = cc_win_fstat(fd, st);
    _close(fd);
    return rc;
}
int cc_win_fstatat(int root, const char *leaf, struct cc_win_stat *st, int flags)
{
    if (!*leaf && (flags & AT_EMPTY_PATH))
        return cc_win_fstat(root, st);
    HANDLE h = cc_win_relative_handle(root, leaf, FILE_READ_ATTRIBUTES, FILE_OPEN, 0, 1);
    if (h == INVALID_HANDLE_VALUE)
        return -1;
    int rc = handle_stat(h, st);
    CloseHandle(h);
    return rc;
}
int cc_win_statx(int root, const char *leaf, int flags, unsigned mask, struct statx *st)
{
    (void)mask;
    struct cc_win_stat value;
    if (cc_win_fstatat(root, leaf, &value, flags))
        return -1;
    st->stx_mask = STATX_BTIME;
    st->stx_btime.tv_sec = value.birth.tv_sec;
    st->stx_btime.tv_nsec = value.birth.tv_nsec;
    return 0;
}
int cc_win_mkdirat(int root, const char *leaf, mode_t mode)
{
    (void)mode;
    HANDLE h = cc_win_relative_handle(root, leaf, file_access(O_DIRECTORY), FILE_CREATE,
                                      FILE_DIRECTORY_FILE, 0);
    if (h == INVALID_HANDLE_VALUE)
        return -1;
    CloseHandle(h);
    return 0;
}
int cc_win_unlinkat(int root, const char *leaf, int flags)
{
    HANDLE h = cc_win_relative_handle(root, leaf, DELETE | FILE_READ_ATTRIBUTES, FILE_OPEN, 0, 0);
    if (h == INVALID_HANDLE_VALUE)
        return -1;
    struct cc_win_stat st;
    int rc = handle_stat(h, &st);
    if (!rc && !!(flags & AT_REMOVEDIR) != !!S_ISDIR(st.st_mode)) {
        errno = EINVAL;
        rc = -1;
    }
    FILE_DISPOSITION_INFO info = {TRUE};
    if (!rc && !SetFileInformationByHandle(h, FileDispositionInfo, &info, sizeof(info)))
        rc = cc_win_errno(GetLastError());
    CloseHandle(h);
    return rc;
}
int cc_win_renameat2(int from, const char *a, int to, const char *b, unsigned flags)
{
    if (!component(b, 1) || (flags & ~RENAME_NOREPLACE)) {
        errno = EINVAL;
        return -1;
    }
    struct cc_win_stat parent;
    if (cc_win_fstat(to, &parent) || !S_ISDIR(parent.st_mode)) {
        errno = ENOTDIR;
        return -1;
    }
    HANDLE h = cc_win_relative_handle(from, a, DELETE | FILE_READ_ATTRIBUTES, FILE_OPEN, 0, 0);
    if (h == INVALID_HANDLE_VALUE)
        return -1;
    wchar_t *name = cc_win_wide(b);
    if (!name) {
        CloseHandle(h);
        return -1;
    }
    size_t bytes = wcslen(name) * sizeof(*name), size = sizeof(FILE_RENAME_INFO) + bytes;
    FILE_RENAME_INFO *info = xmalloc(size);
    info->ReplaceIfExists = !(flags & RENAME_NOREPLACE);
    info->RootDirectory = cc_win_handle(to);
    info->FileNameLength = (DWORD)bytes;
    memcpy(info->FileName, name, bytes);
    /* The native API accepts a handle-relative target. The Win32 wrapper on
     * supported Windows versions can reject a non-NULL RootDirectory. */
    IO_STATUS_BLOCK status;
    NTSTATUS result = nt_set(h, &status, info, (ULONG)size, FileRenameInformation);
    if (result < 0)
        fprintf(stderr, "Windows rename failed (NTSTATUS 0x%08lx, error %lu).\n",
                (unsigned long)result, (unsigned long)nt_error(result));
    int rc = result < 0 ? cc_win_errno(nt_error(result)) : 0;
    free(name);
    free(info);
    CloseHandle(h);
    return rc;
}
int cc_win_renameat(int from, const char *a, int to, const char *b)
{
    return cc_win_renameat2(from, a, to, b, 0);
}
int cc_win_fsync(int fd)
{
    struct cc_win_stat st;
    if (cc_win_fstat(fd, &st))
        return -1;
    /* Windows exposes no unprivileged directory fsync. File bytes and SQLite
     * are flushed; namespace changes use the filesystem's atomic rename and
     * journal. This weaker power-loss guarantee is documented, never applied
     * to Linux. The operation journal reconciles interrupted publications. */
    if (S_ISDIR(st.st_mode))
        return 0;
    return FlushFileBuffers(cc_win_handle(fd)) ? 0 : cc_win_errno(GetLastError());
}
int cc_win_fchmod(int fd, mode_t mode)
{
    (void)mode;
    /* POSIX display bits are not an NTFS ACL. Destination ACLs are inherited;
     * the original security descriptor and attributes stay in the index. */
    struct cc_win_stat st;
    return cc_win_fstat(fd, &st);
}
int cc_win_futimens(int fd, const struct timespec ts[2])
{
    ULARGE_INTEGER a, m;
    a.QuadPart = (uint64_t)ts[0].tv_sec * 10000000 + ts[0].tv_nsec / 100 + 116444736000000000ULL;
    m.QuadPart = (uint64_t)ts[1].tv_sec * 10000000 + ts[1].tv_nsec / 100 + 116444736000000000ULL;
    FILETIME at = {a.LowPart, a.HighPart}, mt = {m.LowPart, m.HighPart};
    return SetFileTime(cc_win_handle(fd), NULL, &at, &mt) ? 0 : cc_win_errno(GetLastError());
}
int cc_win_ftruncate(int fd, int64_t size)
{
    int rc = _chsize_s(fd, size);
    if (rc) {
        errno = rc;
        return -1;
    }
    return 0;
}
int cc_win_flock(int fd, int operation)
{
    if (operation != (LOCK_EX | LOCK_NB)) {
        errno = EINVAL;
        return -1;
    }
    OVERLAPPED offset = {0};
    return LockFileEx(cc_win_handle(fd), LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1,
                      0, &offset)
               ? 0
               : cc_win_errno(GetLastError());
}
int cc_win_fstatvfs(int fd, struct statvfs *fs)
{
    wchar_t *path = cc_win_handle_path(cc_win_handle(fd));
    if (!path)
        return -1;
    ULARGE_INTEGER free_bytes, total;
    BOOL ok = GetDiskFreeSpaceExW(path, &free_bytes, &total, NULL);
    DWORD error = GetLastError();
    free(path);
    if (!ok)
        return cc_win_errno(error);
    fs->f_frsize = 1;
    fs->f_bavail = free_bytes.QuadPart;
    fs->f_blocks = total.QuadPart;
    return 0;
}
ssize_t cc_win_pread(int fd, void *buffer, size_t size, int64_t offset)
{
    /* A bounded read-only mapping uses the inherited read capability. Reopening
     * under a restricted token would incorrectly reject administrator-owned
     * inputs. Mapping also leaves libarchive/libjpeg's shared file offset alone.
     * These reads run in isolated preview workers; an in-page I/O fault is
     * reported by the parent as a failed preview, never a completed copy. */
    if (offset < 0) {
        errno = EINVAL;
        return -1;
    }
    HANDLE input = cc_win_handle(fd);
    LARGE_INTEGER length;
    if (!GetFileSizeEx(input, &length))
        return cc_win_errno(GetLastError());
    if (!size || offset >= length.QuadPart)
        return 0;
    if ((uint64_t)size > (uint64_t)(length.QuadPart - offset))
        size = (size_t)(length.QuadPart - offset);
    if (size > UINT_MAX)
        size = UINT_MAX;
    SYSTEM_INFO system;
    GetSystemInfo(&system);
    uint64_t start = (uint64_t)offset - (uint64_t)offset % system.dwAllocationGranularity;
    size_t skip = (size_t)((uint64_t)offset - start);
    HANDLE mapping = CreateFileMappingW(input, NULL, PAGE_READONLY, 0, 0, NULL);
    if (!mapping)
        return cc_win_errno(GetLastError());
    void *view =
        MapViewOfFile(mapping, FILE_MAP_READ, (DWORD)(start >> 32), (DWORD)start, skip + size);
    DWORD error = view ? 0 : GetLastError();
    if (view) {
        memcpy(buffer, (const unsigned char *)view + skip, size);
        UnmapViewOfFile(view);
    }
    CloseHandle(mapping);
    return view ? (ssize_t)size : cc_win_errno(error);
}
ssize_t cc_win_getrandom(void *buffer, size_t size, unsigned flags)
{
    if (flags || size > ULONG_MAX) {
        errno = EINVAL;
        return -1;
    }
    if (BCryptGenRandom(NULL, buffer, (ULONG)size, BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) {
        errno = EIO;
        return -1;
    }
    return (ssize_t)size;
}
struct CCWinDir {
    int fd, first, ended;
    HANDLE reader;
    _Alignas(FILE_ID_BOTH_DIR_INFO) unsigned char buffer[65536];
    FILE_ID_BOTH_DIR_INFO *next;
    struct cc_win_dirent entry;
};
CCWinDir *cc_win_fdopendir(int fd)
{
    struct cc_win_stat st;
    if (cc_win_fstat(fd, &st) || !S_ISDIR(st.st_mode)) {
        errno = ENOTDIR;
        return NULL;
    }
    if (!ordinary_directory(cc_win_handle(fd)))
        return NULL;
    CCWinDir *dir = xmalloc(sizeof(*dir));
    dir->reader = cc_win_relative_handle(fd, ".", FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
                                         FILE_OPEN, FILE_DIRECTORY_FILE, 0);
    if (dir->reader == INVALID_HANDLE_VALUE) {
        free(dir);
        return NULL;
    }
    dir->fd = fd;
    dir->first = 1;
    return dir;
}
struct cc_win_dirent *cc_win_readdir(CCWinDir *dir)
{
    if (dir->ended)
        return NULL;
    if (!dir->next) {
        if (!GetFileInformationByHandleEx(
                dir->reader, dir->first ? FileIdBothDirectoryRestartInfo : FileIdBothDirectoryInfo,
                dir->buffer, sizeof(dir->buffer))) {
            DWORD error = GetLastError();
            dir->ended = 1;
            if (error != ERROR_NO_MORE_FILES)
                cc_win_errno(error);
            return NULL;
        }
        dir->first = 0;
        dir->next = (FILE_ID_BOTH_DIR_INFO *)dir->buffer;
    }
    FILE_ID_BOTH_DIR_INFO *entry = dir->next;
    dir->next = entry->NextEntryOffset
                    ? (FILE_ID_BOTH_DIR_INFO *)((char *)entry + entry->NextEntryOffset)
                    : NULL;
    int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, entry->FileName,
                                     (int)(entry->FileNameLength / sizeof(wchar_t)),
                                     dir->entry.d_name, sizeof(dir->entry.d_name) - 1, NULL, NULL);
    if (!length) {
        dir->ended = 1;
        errno = EILSEQ;
        return NULL;
    }
    dir->entry.d_name[length] = 0;
    return &dir->entry;
}
int cc_win_closedir(CCWinDir *dir)
{
    CloseHandle(dir->reader);
    int rc = _close(dir->fd);
    free(dir);
    return rc;
}
char *cc_win_realpath(const char *path, char *resolved)
{
    /* All application callers request allocated paths, including long paths. */
    if (resolved) {
        errno = EINVAL;
        return NULL;
    }
    wchar_t *wide = absolute_path(path);
    if (!wide)
        return NULL;
    HANDLE h = CreateFileW(wide, FILE_READ_ATTRIBUTES,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    free(wide);
    if (h == INVALID_HANDLE_VALUE) {
        cc_win_errno(GetLastError());
        return NULL;
    }
    wchar_t *final = cc_win_handle_path(h);
    CloseHandle(h);
    if (!final)
        return NULL;
    wchar_t *name = final;
    if (!wcsncmp(final, L"\\\\?\\UNC\\", 8)) {
        name = final + 6;
        name[0] = '\\';
    } else if (!wcsncmp(final, L"\\\\?\\", 4))
        name = final + 4;
    char *out = cc_win_utf8(name);
    free(final);
    if (out)
        for (char *p = out; *p; p++)
            if (*p == '\\')
                *p = '/';
    return out;
}
char *cc_win_getcwd(char *buffer, size_t size)
{
    DWORD length = GetCurrentDirectoryW(0, NULL);
    wchar_t *wide = xmalloc((size_t)length * sizeof(*wide));
    if (!GetCurrentDirectoryW(length, wide)) {
        free(wide);
        cc_win_errno(GetLastError());
        return NULL;
    }
    char *out = cc_win_utf8(wide);
    free(wide);
    if (!out)
        return NULL;
    for (char *p = out; *p; p++)
        if (*p == '\\')
            *p = '/';
    if (!buffer)
        return out;
    if (strlen(out) >= size) {
        free(out);
        errno = ERANGE;
        return NULL;
    }
    strcpy(buffer, out);
    free(out);
    return buffer;
}
FILE *cc_win_fopen(const char *path, const char *mode)
{
    wchar_t *wide = absolute_path(path), *m = cc_win_wide(mode);
    FILE *result = wide && m ? _wfopen(wide, m) : NULL;
    free(wide);
    free(m);
    return result;
}
char *cc_win_strcasestr(const char *text, const char *needle)
{
    size_t length = strlen(needle);
    for (; *text; text++)
        if (!_strnicmp(text, needle, length))
            return (char *)text;
    return length ? NULL : (char *)text;
}
int cc_win_descendant(const char *path, const char *parent)
{
    if (!*parent)
        return 1;
    wchar_t *p = cc_win_wide(path), *r = cc_win_wide(parent);
    if (!p || !r) {
        free(p);
        free(r);
        return 0;
    }
    size_t n = wcslen(r), len = wcslen(p);
    int match = len >= n && CompareStringOrdinal(p, (int)n, r, (int)n, TRUE) == CSTR_EQUAL &&
                (len == n || r[n - 1] == '/' || p[n] == '/');
    free(p);
    free(r);
    return match;
}
char *cc_win_parent_path(const char *path)
{
    char *out = xstrdup(path), *p = strrchr(out, '/');
    if (p && p == out + 2 && out[1] == ':')
        p[1] = 0;
    else if (p)
        *p = 0;
    else
        *out = 0;
    return out;
}
struct tm *cc_win_gmtime_r(const time_t *value, struct tm *out)
{
    return gmtime_s(out, value) ? NULL : out;
}
struct tm *cc_win_localtime_r(const time_t *value, struct tm *out)
{
    return localtime_s(out, value) ? NULL : out;
}
ssize_t cc_win_getline(char **line, size_t *capacity, FILE *file)
{
    size_t size = 0;
    int ch;
    while ((ch = fgetc(file)) != EOF) {
        if (!*line || size + 1 >= *capacity) {
            size_t next = *line && *capacity ? *capacity * 2 : 4096;
            if (next <= size || next > (size_t)INT64_MAX) {
                errno = EOVERFLOW;
                return -1;
            }
            char *p = realloc(*line, next);
            if (!p) {
                errno = ENOMEM;
                return -1;
            }
            *line = p;
            *capacity = next;
        }
        (*line)[size++] = (char)ch;
        if (ch == '\n')
            break;
    }
    if (!size)
        return -1;
    (*line)[size] = 0;
    return (ssize_t)size;
}
int cc_win_validate_roots(const char *source, const char *destination, char *error, size_t size)
{
    char *dest = cc_win_realpath(destination, NULL),
         *src = source ? cc_win_realpath(source, NULL) : NULL;
    int ok = 0;
    struct cc_win_stat a, b;
    if (!dest || (source && !src))
        snprintf(error, size, "Cannot resolve source/destination: %s", strerror(errno));
    else if (cc_win_stat(dest, &b) || !S_ISDIR(b.st_mode))
        snprintf(error, size, "Destination must be a directory.");
    else if (source && (cc_win_stat(src, &a) || !S_ISDIR(a.st_mode)))
        snprintf(error, size, "Source must be a directory.");
    else if (source && a.st_dev == b.st_dev && a.st_ino == b.st_ino)
        snprintf(error, size, "Source and destination must be different folders.");
    else if (source && cc_win_descendant(src, dest))
        snprintf(error, size, "Destination must not contain the source tree.");
    else
        ok = 1;
    free(src);
    free(dest);
    return ok;
}
