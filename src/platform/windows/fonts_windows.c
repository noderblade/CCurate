#define CCURATE_WINDOWS_NATIVE
#include "platform/windows/fonts_windows.h"
#include "curator.h"
#include "platform/windows/native_windows.h"

void cc_win_font_dirs(FcConfig *config)
{
    wchar_t path[32768];
    UINT n = GetWindowsDirectoryW(path, 32768);
    if (n && n < 32760) {
        wcscat(path, L"\\Fonts");
        char *name = cc_win_utf8(path);
        if (name) {
            FcConfigAppFontAddDir(config, (const FcChar8 *)name);
            free(name);
        }
    }
    DWORD size = GetEnvironmentVariableW(L"LOCALAPPDATA", path, 32768);
    if (size && size < 32730) {
        wcscat(path, L"\\Microsoft\\Windows\\Fonts");
        char *name = cc_win_utf8(path);
        if (name) {
            FcConfigAppFontAddDir(config, (const FcChar8 *)name);
            free(name);
        }
    }
}
