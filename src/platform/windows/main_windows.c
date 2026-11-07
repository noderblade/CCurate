#define CCURATE_WINDOWS_NATIVE
#include "curator.h"
#include "platform/windows/diagnostics_windows.h"
#include "platform/windows/native_windows.h"
#include <shellapi.h>
#ifdef CCURATE_GUI
#include <Raven.h>
static void startup_error(RError code, RSeverity severity, const char *detail)
{
    fprintf(stderr, "Raven %d/%d: %s\n", code, severity, detail ? detail : "");
}
#endif

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
        _wputenv_s(name, path);
}
void cc_win_runtime_init(void)
{
#ifdef CCURATE_GUI
    Rerror_set_callback(startup_error);
#endif
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    SetConsoleOutputCP(CP_UTF8);
    _set_fmode(_O_BINARY);
    for (int fd = 0; fd < 3; fd++)
        if (_get_osfhandle(fd) != -1 && _get_osfhandle(fd) != -2)
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
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    int argc = 0;
    wchar_t **wide = CommandLineToArgvW(GetCommandLineW(), &argc);
    int debug = wide && argc > 1 && !wcscmp(wide[1], L"--debug");
    int first = debug ? 2 : 1;
    int worker =
        wide && argc > first &&
        (!wcscmp(wide[first], L"--preview-worker") || !wcscmp(wide[first], L"--open-worker"));
    int graphical = 0;
#ifdef CCURATE_GUI
    graphical = !worker && (argc <= first ||
                            (wcscmp(wide[first], L"--cli") && wcscmp(wide[first], L"--help") &&
                             wcscmp(wide[first], L"--version")));
#endif
    if (!worker)
        cc_win_diagnostics_start(debug, graphical);
    cc_win_runtime_init();
    if (!wide) {
        cc_win_startup_note("ERROR: CommandLineToArgvW failed.");
        cc_win_diagnostics_finish(2);
        return 2;
    }
    int count = argc - debug;
    char **argv = xmalloc(((size_t)count + 1) * sizeof(*argv));
    for (int i = 0; i < count; i++) {
        argv[i] = cc_win_utf8(wide[i && debug ? i + 1 : i]);
        if (!argv[i]) {
            for (int j = 0; j < i; j++)
                free(argv[j]);
            free(argv);
            LocalFree(wide);
            if (!worker) {
                cc_win_startup_note("ERROR: Cannot convert command-line arguments to UTF-8.");
                cc_win_diagnostics_finish(2);
            }
            return 2;
        }
    }
    LocalFree(wide);
    if (!worker)
        cc_win_startup_note("Runtime resources configured; entering the application.");
    /* Inherited standard handles are CRT descriptors 0, 1 and 2 in workers. */
    int rc = ccurate_main(count, argv);
    for (int i = 0; i < count; i++)
        free(argv[i]);
    free(argv);
    if (!worker)
        cc_win_diagnostics_finish(rc);
    return rc;
}
