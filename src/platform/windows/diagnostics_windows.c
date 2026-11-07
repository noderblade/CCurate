#define CCURATE_WINDOWS_NATIVE
#include "platform/windows/diagnostics_windows.h"
#include "platform/windows/native_windows.h"
#include <fcntl.h>
#include <io.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef CCURATE_BUILD_ID
#define CCURATE_BUILD_ID "unknown"
#endif
#define LOG_LIMIT (8 * 1024 * 1024)
static HANDLE logfile = INVALID_HANDLE_VALUE, console = INVALID_HANDLE_VALUE;
static HANDLE reader, pump;
static wchar_t log_path[32768];
static int stderr_fd = -1, saved_stderr = -1, capturing, allocated_console, graphical_start,
           had_output;
static LONG ready;
static LPTOP_LEVEL_EXCEPTION_FILTER previous_filter;

static int valid_handle(HANDLE handle)
{
    DWORD flags;
    return handle && handle != INVALID_HANDLE_VALUE && (intptr_t)handle != -2 &&
           GetHandleInformation(handle, &flags);
}
static HANDLE stream_handle(FILE *stream)
{
    int fd = _fileno(stream);
    return fd >= 0 ? (HANDLE)_get_osfhandle(fd) : INVALID_HANDLE_VALUE;
}
static void write_handle(HANDLE handle, const char *text, DWORD size)
{
    while (valid_handle(handle) && size) {
        DWORD written = 0;
        if (!WriteFile(handle, text, size, &written, NULL) || !written)
            break;
        text += written;
        size -= written;
    }
}
static void emit(const char *text, DWORD size)
{
    write_handle(logfile, text, size);
    if (logfile != INVALID_HANDLE_VALUE)
        FlushFileBuffers(logfile);
    write_handle(console, text, size);
}
static DWORD WINAPI drain_stderr(void *unused)
{
    (void)unused;
    char bytes[4096];
    DWORD count, total = 0;
    while (ReadFile(reader, bytes, sizeof(bytes), &count, NULL) && count) {
        if (total < LOG_LIMIT) {
            DWORD keep = count < LOG_LIMIT - total ? count : LOG_LIMIT - total;
            write_handle(logfile, bytes, keep);
            total += keep;
            if (total == LOG_LIMIT) {
                const char *notice =
                    "\nStartup log reached 8 MiB; further stderr is console-only.\n";
                write_handle(logfile, notice, (DWORD)strlen(notice));
            }
            if (logfile != INVALID_HANDLE_VALUE)
                FlushFileBuffers(logfile);
        }
        write_handle(console, bytes, count);
    }
    return 0;
}
void cc_win_startup_note(const char *message)
{
    SYSTEMTIME now;
    char line[4096];
    GetSystemTime(&now);
    int n = snprintf(line, sizeof(line), "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ [startup] %s\n",
                     now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond,
                     now.wMilliseconds, message);
    if (n < 0)
        return;
    DWORD length = (DWORD)((size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1);
    if (capturing)
        fwrite(line, 1, length, stderr);
    else
        emit(line, length);
}
void cc_win_startup_ready(void)
{
    if (!InterlockedExchange(&ready, 1))
        cc_win_startup_note("First GUI frame completed.");
}
static int create_log(const wchar_t *directory)
{
    if (!directory || !*directory || wcslen(directory) > 32000)
        return 0;
    int rc = SHCreateDirectoryExW(NULL, directory, NULL);
    if (rc != ERROR_SUCCESS && rc != ERROR_ALREADY_EXISTS && rc != ERROR_FILE_EXISTS)
        return 0;
    SYSTEMTIME now;
    GetSystemTime(&now);
    swprintf(log_path, 32768, L"%ls\\startup-%04u%02u%02u-%02u%02u%02u-%03u-%lu.log", directory,
             now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond,
             now.wMilliseconds, (unsigned long)GetCurrentProcessId());
    logfile = CreateFileW(log_path, FILE_APPEND_DATA | SYNCHRONIZE, FILE_SHARE_READ, NULL,
                          CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    return logfile != INVALID_HANDLE_VALUE;
}
static void error_dialog(const wchar_t *reason)
{
    wchar_t message[32768];
    swprintf(
        message, 32768,
        L"%ls\n\nDiagnostic log:\n%ls\n\nYou can also run CCurate-Debug.cmd from the extracted "
        L"package to collect the Windows exit code and loader diagnostics.",
        reason, *log_path ? log_path : L"Unavailable; use the diagnostic launcher.");
    MessageBoxW(NULL, message, L"CCurate - startup failed", MB_OK | MB_ICONERROR);
}
static LONG WINAPI unhandled(EXCEPTION_POINTERS *exception)
{
    char message[256];
    int n = snprintf(message, sizeof(message),
                     "\nUnhandled Windows exception 0x%08lX at %p. The process cannot continue.\n",
                     (unsigned long)exception->ExceptionRecord->ExceptionCode,
                     exception->ExceptionRecord->ExceptionAddress);
    if (n > 0)
        emit(message, (DWORD)n);
    if (graphical_start && !had_output && !allocated_console)
        error_dialog(L"CCurate stopped because of an unhandled Windows exception.");
    return EXCEPTION_EXECUTE_HANDLER;
}
static void describe_wide(const char *name, const wchar_t *value)
{
    char utf8[3072], line[4096];
    if (!value || !WideCharToMultiByte(CP_UTF8, 0, value, -1, utf8, sizeof(utf8), NULL, NULL))
        strcpy(utf8, "(unavailable or too long)");
    snprintf(line, sizeof(line), "%s: %s", name, utf8);
    cc_win_startup_note(line);
}
void cc_win_diagnostics_start(int debug, int graphical)
{
    graphical_start = graphical;
    /* Attach for ordinary GUI invocations too. Preserve redirected descriptors;
     * never replace a caller's stdout/stderr pipe with CONOUT$. */
    int have_out = valid_handle(stream_handle(stdout));
    int have_err = valid_handle(stream_handle(stderr));
    had_output = have_out || have_err;
    if (!GetConsoleCP()) {
        if (!AttachConsole(ATTACH_PARENT_PROCESS) && debug && !had_output)
            allocated_console = AllocConsole() != 0;
    }
    if (GetConsoleCP()) {
        FILE *stream;
        if (!have_out)
            freopen_s(&stream, "CONOUT$", "w", stdout);
        if (!have_err)
            freopen_s(&stream, "CONOUT$", "w", stderr);
        if (!valid_handle(stream_handle(stdin)))
            freopen_s(&stream, "CONIN$", "r", stdin);
        SetConsoleOutputCP(CP_UTF8);
        had_output = 1;
    }
    /* UCRT's unattached standard streams can have fileno -2. Reopening
     * them may assign descriptors other than 0/1/2: always follow the FILE's
     * actual descriptor, including when redirecting stderr to the log pipe. */
    FILE *streams[] = {stdin, stdout, stderr};
    DWORD handles[] = {STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE};
    for (int i = 0; i < 3; i++) {
        if (!valid_handle(stream_handle(streams[i]))) {
            FILE *stream;
            freopen_s(&stream, "NUL", i ? "w" : "r", streams[i]);
        }
        HANDLE handle = stream_handle(streams[i]);
        if (valid_handle(handle)) {
            _setmode(_fileno(streams[i]), _O_BINARY);
            SetStdHandle(handles[i], handle);
        }
    }
    stderr_fd = _fileno(stderr);
    if (valid_handle(stream_handle(stderr))) {
        saved_stderr = _dup(stderr_fd);
        DuplicateHandle(GetCurrentProcess(), stream_handle(stderr), GetCurrentProcess(), &console,
                        0, FALSE, DUPLICATE_SAME_ACCESS);
    }
    wchar_t folder[32768];
    DWORD length = GetEnvironmentVariableW(L"CCURATE_LOG_DIR", folder, 32768);
    int opened = length && length < 32768 && create_log(folder);
    int requested_failed = length && !opened;
    if (!opened &&
        SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, SHGFP_TYPE_CURRENT, folder))) {
        wcscat(folder, L"\\CCurate\\Logs");
        opened = create_log(folder);
    }
    if (!opened && GetTempPathW(32000, folder)) {
        wcscat(folder, L"CCurate\\Logs");
        opened = create_log(folder);
    }
    if (!opened)
        *log_path = 0;
    /* Workers never enter here. The pipe is non-inheritable and stdout remains
     * untouched, including binary preview packets and CLI output. */
    HANDLE writer = NULL;
    if (opened && stderr_fd >= 0 && CreatePipe(&reader, &writer, NULL, 0)) {
        int fd = _open_osfhandle((intptr_t)writer, _O_WRONLY | _O_BINARY | _O_NOINHERIT);
        if (fd >= 0) {
            pump = CreateThread(NULL, 0, drain_stderr, NULL, 0, NULL);
            if (pump && !_dup2(fd, stderr_fd)) {
                capturing = 1;
                SetHandleInformation(stream_handle(stderr), HANDLE_FLAG_INHERIT, 0);
                SetStdHandle(STD_ERROR_HANDLE, stream_handle(stderr));
                setvbuf(stderr, NULL, _IONBF, 0);
            }
            _close(fd);
        } else
            CloseHandle(writer);
    }
    previous_filter = SetUnhandledExceptionFilter(unhandled);
    char line[256];
    snprintf(line, sizeof(line), "CCurate 0.1.0; build %s; Windows x64; PID %lu; mode=%s; debug=%s",
             CCURATE_BUILD_ID, (unsigned long)GetCurrentProcessId(), graphical ? "GUI" : "CLI",
             debug ? "yes" : "no");
    cc_win_startup_note(line);
    typedef LONG(WINAPI * VersionFn)(OSVERSIONINFOW *);
    VersionFn version =
        (VersionFn)(void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
    OSVERSIONINFOW os = {.dwOSVersionInfoSize = sizeof(os)};
    if (version && !version(&os)) {
        snprintf(line, sizeof(line), "Windows version: %lu.%lu build %lu",
                 (unsigned long)os.dwMajorVersion, (unsigned long)os.dwMinorVersion,
                 (unsigned long)os.dwBuildNumber);
        cc_win_startup_note(line);
    }
    describe_wide("Startup log", *log_path ? log_path : L"unavailable");
    if (!capturing)
        cc_win_startup_note("WARNING: stderr file capture is unavailable.");
    if (requested_failed)
        cc_win_startup_note(
            "WARNING: requested log directory is unavailable; using a fallback when possible.");
    GetModuleFileNameW(NULL, folder, 32768);
    describe_wide("Executable", folder);
    length = GetCurrentDirectoryW(32768, folder);
    if (length && length < 32768)
        describe_wide("Working directory", folder);
    if (debug && !GetEnvironmentVariableW(L"VK_LOADER_DEBUG", folder, 32768))
        _wputenv_s(L"VK_LOADER_DEBUG", L"error,warn,driver");
    if (debug)
        _wputenv_s(L"CCURATE_WINDOWS_DIAGNOSTICS", L"1");
    const wchar_t *variables[] = {L"VK_LOADER_DEBUG", L"VK_DRIVER_FILES", L"VK_ICD_FILENAMES"};
    const char *names[] = {"VK_LOADER_DEBUG", "VK_DRIVER_FILES", "VK_ICD_FILENAMES"};
    for (int i = 0; i < 3; i++) {
        DWORD n = GetEnvironmentVariableW(variables[i], folder, 32768);
        describe_wide(names[i], n && n < 32768 ? folder : L"(not set)");
    }
    if (graphical || debug) {
        DISPLAY_DEVICEW device = {.cb = sizeof(device)};
        for (DWORD i = 0; EnumDisplayDevicesW(NULL, i, &device, 0); i++) {
            describe_wide("Display adapter", device.DeviceString);
            device.cb = sizeof(device);
        }
    }
}
void cc_win_diagnostics_finish(int status)
{
    char line[256];
    snprintf(line, sizeof(line), "Application exited: status=%d (0x%08X); first_frame=%s", status,
             (unsigned)status, ready ? "yes" : "no");
    cc_win_startup_note(line);
    fflush(stderr);
    if (capturing) {
        capturing = 0;
        if (saved_stderr >= 0)
            _dup2(saved_stderr, stderr_fd);
        else {
            FILE *stream;
            freopen_s(&stream, "NUL", "w", stderr);
        }
        SetStdHandle(STD_ERROR_HANDLE, stream_handle(stderr));
    }
    if (saved_stderr >= 0)
        _close(saved_stderr);
    if (pump) {
        if (WaitForSingleObject(pump, 2000) != WAIT_OBJECT_0) {
            CancelSynchronousIo(pump);
            WaitForSingleObject(pump, 1000);
        }
        CloseHandle(pump);
    }
    if (reader)
        CloseHandle(reader);
    SetUnhandledExceptionFilter(previous_filter);
    if (status && graphical_start && !had_output)
        error_dialog(L"CCurate could not start. The diagnostic log contains the error details.");
    if (allocated_console) {
        fputs("\nPress Enter to close this diagnostic console...", stderr);
        fflush(stderr);
        char input[8];
        fgets(input, sizeof(input), stdin);
    }
    if (logfile != INVALID_HANDLE_VALUE)
        CloseHandle(logfile);
    if (console != INVALID_HANDLE_VALUE)
        CloseHandle(console);
}
