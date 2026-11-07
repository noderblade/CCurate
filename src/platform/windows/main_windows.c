#define CCURATE_WINDOWS_NATIVE
#include "curator.h"
#include "platform/windows/native_windows.h"
#include <shellapi.h>

static void runtime_variable(const wchar_t *name, const wchar_t *tail)
{
    wchar_t path[32768];
    DWORD n = GetModuleFileNameW(NULL, path, 32768);
    if (!n || n >= 32768)
        return;
    wchar_t *slash = wcsrchr(path, '\\');
    if (!slash || (size_t)(slash - path) + wcslen(tail) + 2 >= 32768)
        return;
    wcscpy(slash + 1, tail);
    if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES)
        SetEnvironmentVariableW(name, path);
}
void cc_win_runtime_init(void)
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    SetConsoleOutputCP(CP_UTF8);
    _set_fmode(_O_BINARY);
    for (int fd = 0; fd < 3; fd++)
        if (_get_osfhandle(fd) != -1)
            _setmode(fd, _O_BINARY);
    runtime_variable(L"MAGIC", L"share\\misc\\magic.mgc");
    runtime_variable(L"FONTCONFIG_FILE", L"etc\\fonts\\fonts.conf");
    runtime_variable(L"FONTCONFIG_PATH", L"etc\\fonts");
    runtime_variable(L"GDK_PIXBUF_MODULE_FILE", L"lib\\gdk-pixbuf-2.0\\2.10.0\\loaders.cache");
}
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR line, int show)
{
    (void)instance;
    (void)previous;
    (void)line;
    (void)show;
    cc_win_runtime_init();
    int argc;
    wchar_t **wide = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!wide)
        return 2;
    char **argv = xmalloc(((size_t)argc + 1) * sizeof(*argv));
    for (int i = 0; i < argc; i++) {
        argv[i] = cc_win_utf8(wide[i]);
        if (!argv[i]) {
            LocalFree(wide);
            return 2;
        }
    }
    LocalFree(wide);
    /* Inherited standard handles are CRT descriptors 0, 1 and 2 in workers. */
    int rc = ccurate_main(argc, argv);
    for (int i = 0; i < argc; i++)
        free(argv[i]);
    free(argv);
    return rc;
}
