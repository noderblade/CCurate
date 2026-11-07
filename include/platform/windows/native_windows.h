#ifndef CCURATE_NATIVE_WINDOWS_H
#define CCURATE_NATIVE_WINDOWS_H
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0a00
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <stddef.h>
#include <wchar.h>
#include <windows.h>

/* Allocated conversion results belong to the caller. Invalid encodings fail. */
wchar_t *cc_win_wide(const char *utf8);
char *cc_win_utf8(const wchar_t *wide);
wchar_t *cc_win_handle_path(HANDLE handle);
int cc_win_errno(DWORD error);
int cc_win_fd(HANDLE handle, int flags);
HANDLE cc_win_handle(int fd);
HANDLE cc_win_relative_handle(int root, const char *leaf, ACCESS_MASK access, ULONG disposition,
                              ULONG options, int allow_reparse);
void cc_win_runtime_init(void);
int ccurate_main(int argc, char **argv);
#endif
