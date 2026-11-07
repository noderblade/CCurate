#define CCURATE_WINDOWS_NATIVE
#include "curator.h"
#include "platform/windows/native_windows.h"
#include <aclapi.h>
#include <sddl.h>
#include <winioctl.h>

static FILE_STREAM_INFO *streams(int fd, int *unsupported)
{
    *unsupported = 0;
    for (DWORD size = 4096; size <= 4 * 1024 * 1024; size *= 2) {
        FILE_STREAM_INFO *info = xmalloc(size);
        if (GetFileInformationByHandleEx(cc_win_handle(fd), FileStreamInfo, info, size))
            return info;
        DWORD error = GetLastError();
        free(info);
        if (error == ERROR_HANDLE_EOF || error == ERROR_INVALID_PARAMETER ||
            error == ERROR_NOT_SUPPORTED || error == ERROR_INVALID_FUNCTION) {
            /* Filesystems without named-stream support have no extra content. */
            *unsupported = 1;
            return NULL;
        }
        if (error != ERROR_MORE_DATA && error != ERROR_INSUFFICIENT_BUFFER) {
            cc_win_errno(error);
            return NULL;
        }
    }
    errno = EOVERFLOW;
    return NULL;
}
static char *stream_name(const FILE_STREAM_INFO *info)
{
    size_t count = info->StreamNameLength / sizeof(wchar_t);
    wchar_t *wide = xmalloc((count + 1) * sizeof(*wide));
    memcpy(wide, info->StreamName, info->StreamNameLength);
    char *name = cc_win_utf8(wide);
    free(wide);
    return name;
}
int cc_win_content_supported(int fd, char *error, size_t size)
{
    int unsupported;
    FILE_STREAM_INFO *data = streams(fd, &unsupported);
    if (!data) {
        if (unsupported)
            return 1;
        snprintf(error, size, "Cannot inspect alternate data streams: %s", strerror(errno));
        return 0;
    }
    int ok = 1;
    for (FILE_STREAM_INFO *s = data; s;) {
        char *name = stream_name(s);
        if (!name || strcmp(name, "::$DATA")) {
            snprintf(error, size,
                     "Alternate data stream %s is not retained by this Windows version; copying is "
                     "blocked",
                     name ? name : "[invalid name]");
            ok = 0;
        }
        free(name);
        if (!ok || !s->NextEntryOffset)
            break;
        s = (FILE_STREAM_INFO *)((char *)s + s->NextEntryOffset);
    }
    free(data);
    return ok;
}
static void attribute(Buf *b, const char *name, const char *value, int *count)
{
    char *key = b64(name, strlen(name)), *bytes = b64(value, strlen(value));
    badd(b, "%s{\"name_b64\":\"%s\",\"value_b64\":\"%s\"}", (*count)++ ? "," : "", key, bytes);
    free(key);
    free(bytes);
}
char *cc_win_attributes(int fd, char error[256])
{
    Buf b = {0};
    int count = 0;
    badd(&b, "[");
    struct cc_win_stat st;
    if (!cc_win_fstat(fd, &st)) {
        char flags[32];
        snprintf(flags, sizeof(flags), "0x%08lx", (unsigned long)st.attributes);
        attribute(&b, "windows.file_attributes", flags, &count);
    } else
        snprintf(error, 256, "Windows attributes: %s", strerror(errno));
    PSECURITY_DESCRIPTOR sd = NULL;
    DWORD status = GetSecurityInfo(cc_win_handle(fd), SE_FILE_OBJECT,
                                   OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION |
                                       DACL_SECURITY_INFORMATION,
                                   NULL, NULL, NULL, NULL, &sd);
    if (!status) {
        LPWSTR sddl = NULL;
        if (ConvertSecurityDescriptorToStringSecurityDescriptorW(
                sd, SDDL_REVISION_1,
                OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                &sddl, NULL)) {
            char *value = cc_win_utf8(sddl);
            if (value) {
                attribute(&b, "windows.security_descriptor_sddl", value, &count);
                free(value);
            }
            LocalFree(sddl);
        } else
            snprintf(error, 256, "Windows security descriptor: error %lu",
                     (unsigned long)GetLastError());
        LocalFree(sd);
    } else
        snprintf(error, 256, "Windows security descriptor: error %lu", (unsigned long)status);
    int unsupported;
    FILE_STREAM_INFO *data = streams(fd, &unsupported);
    if (data) {
        Buf list = {0};
        badd(&list, "[");
        int entries = 0;
        for (FILE_STREAM_INFO *s = data; s;) {
            char *name = stream_name(s), *json = json_string(name ? name : "[invalid name]");
            badd(&list, "%s{\"name\":%s,\"size\":%lld}", entries++ ? "," : "", json,
                 (long long)s->StreamSize.QuadPart);
            free(json);
            free(name);
            if (!s->NextEntryOffset)
                break;
            s = (FILE_STREAM_INFO *)((char *)s + s->NextEntryOffset);
        }
        badd(&list, "]");
        attribute(&b, "windows.streams", list.s, &count);
        bfree(&list);
        free(data);
    } else if (!unsupported)
        snprintf(error, 256, "Windows streams: %s", strerror(errno));
    badd(&b, "]");
    return b.s;
}
ssize_t cc_win_readlinkat(int root, const char *leaf, char *buffer, size_t size)
{
    HANDLE h = cc_win_relative_handle(root, leaf, FILE_READ_ATTRIBUTES, 1, 0, 1);
    if (h == INVALID_HANDLE_VALUE)
        return -1;
    unsigned char raw[MAXIMUM_REPARSE_DATA_BUFFER_SIZE];
    DWORD got = 0;
    BOOL ok = DeviceIoControl(h, FSCTL_GET_REPARSE_POINT, NULL, 0, raw, sizeof(raw), &got, NULL);
    DWORD error = GetLastError();
    CloseHandle(h);
    if (!ok)
        return cc_win_errno(error);
    if (got < 8) {
        errno = EIO;
        return -1;
    }
    DWORD tag;
    memcpy(&tag, raw, 4);
    char *text = NULL;
    if (tag == IO_REPARSE_TAG_SYMLINK || tag == IO_REPARSE_TAG_MOUNT_POINT) {
        size_t header = tag == IO_REPARSE_TAG_SYMLINK ? 20 : 16;
        WORD offset = 0, length = 0;
        if (got >= header) {
            memcpy(&offset, raw + 12, 2);
            memcpy(&length, raw + 14, 2);
            if (!length) {
                memcpy(&offset, raw + 8, 2);
                memcpy(&length, raw + 10, 2);
            }
            if (header + offset + length <= got && !(length % 2) && !(offset % 2)) {
                wchar_t *wide = xmalloc((size_t)length + sizeof(wchar_t));
                memcpy(wide, raw + header + offset, length);
                text = cc_win_utf8(wide);
                free(wide);
            }
        }
    }
    if (!text) {
        char *bytes = b64(raw, got);
        text = fmt("Windows reparse point 0x%08lx; raw_base64=%s", (unsigned long)tag, bytes);
        free(bytes);
    }
    size_t n = strlen(text);
    if (n > size) {
        free(text);
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(buffer, text, n);
    free(text);
    return (ssize_t)n;
}
