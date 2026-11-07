#define CCURATE_WINDOWS_NATIVE
#include "platform/windows/process_windows.h"
#include "curator.h"
#include <sddl.h>

static wchar_t *command_line(const wchar_t *exe, const char *const *args)
{
    /* Windows argv quoting, never cmd.exe or PowerShell interpolation. */
    Buf text = {0};
    char *name = cc_win_utf8(exe);
    for (int index = -1; index < 0 || args[index]; index++) {
        const char *arg = index < 0 ? name : args[index];
        if (index >= 0)
            badd(&text, " ");
        badd(&text, "\"");
        size_t slashes = 0;
        for (const char *p = arg;; p++) {
            if (*p == '\\') {
                slashes++;
                continue;
            }
            size_t count = (*p == '"' || !*p) ? slashes * 2 : slashes;
            for (size_t i = 0; i < count; i++)
                badd(&text, "\\");
            slashes = 0;
            if (!*p)
                break;
            if (*p == '"')
                badd(&text, "\\");
            badd(&text, "%c", *p);
        }
        badd(&text, "\"");
    }
    wchar_t *wide = cc_win_wide(text.s);
    bfree(&text);
    free(name);
    return wide;
}
int cc_win_elevated(void)
{
    HANDLE token = NULL;
    TOKEN_ELEVATION elevation;
    DWORD size;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return 1;
    BOOL ok = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size);
    CloseHandle(token);
    return !ok || elevation.TokenIsElevated;
}
static HANDLE low_token(void)
{
    HANDLE current = NULL, token = NULL;
    PSID sid = NULL;
    if (!OpenProcessToken(
            GetCurrentProcess(),
            TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_ASSIGN_PRIMARY | TOKEN_ADJUST_DEFAULT, &current))
        return NULL;
    if (!CreateRestrictedToken(current, DISABLE_MAX_PRIVILEGE | LUA_TOKEN, 0, NULL, 0, NULL, 0,
                               NULL, &token)) {
        DWORD error = GetLastError();
        CloseHandle(current);
        SetLastError(error);
        return NULL;
    }
    CloseHandle(current);
    if (!ConvertStringSidToSidW(L"S-1-16-4096", &sid)) {
        DWORD error = GetLastError();
        CloseHandle(token);
        SetLastError(error);
        return NULL;
    }
    TOKEN_MANDATORY_LABEL label = {{sid, SE_GROUP_INTEGRITY}};
    BOOL ok =
        SetTokenInformation(token, TokenIntegrityLevel, &label, sizeof(label) + GetLengthSid(sid));
    DWORD error = GetLastError();
    LocalFree(sid);
    if (!ok) {
        CloseHandle(token);
        SetLastError(error);
        return NULL;
    }
    return token;
}
int cc_win_spawn(CCWinProcess *child, int input, const char *const *arguments, int restricted)
{
    memset(child, 0, sizeof(*child));
    SECURITY_ATTRIBUTES security = {sizeof(security), NULL, TRUE};
    HANDLE writes[2] = {NULL, NULL}, source = NULL, token = NULL;
    PROCESS_INFORMATION process = {0};
    STARTUPINFOEXW start = {0};
    wchar_t exe[32768], directory[32768], *command = NULL;
    DWORD error = ERROR_GEN_FAILURE;
    SIZE_T bytes = 0;
    int attributes_ready = 0;
    DWORD length = GetModuleFileNameW(NULL, exe, 32768);
    if (!length || length >= 32768)
        goto failure;
    wcscpy(directory, exe);
    wchar_t *slash = wcsrchr(directory, '\\');
    if (slash)
        *slash = 0;
    command = command_line(exe, arguments);
    if (!command)
        goto failure;
    if (!CreatePipe(&child->output, &writes[0], &security, 65536) ||
        !CreatePipe(&child->errors, &writes[1], &security, 65536) ||
        !SetHandleInformation(child->output, HANDLE_FLAG_INHERIT, 0) ||
        !SetHandleInformation(child->errors, HANDLE_FLAG_INHERIT, 0) ||
        !DuplicateHandle(GetCurrentProcess(), cc_win_handle(input), GetCurrentProcess(), &source, 0,
                         TRUE, DUPLICATE_SAME_ACCESS))
        goto failure;
    InitializeProcThreadAttributeList(NULL, 1, 0, &bytes);
    start.lpAttributeList = xmalloc(bytes);
    if (!InitializeProcThreadAttributeList(start.lpAttributeList, 1, 0, &bytes))
        goto failure;
    attributes_ready = 1;
    HANDLE inherited[] = {source, writes[0], writes[1]};
    if (!UpdateProcThreadAttribute(start.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                   inherited, sizeof(inherited), NULL, NULL))
        goto failure;
    start.StartupInfo.cb = sizeof(start);
    start.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    start.StartupInfo.hStdInput = source;
    start.StartupInfo.hStdOutput = writes[0];
    start.StartupInfo.hStdError = writes[1];
    DWORD flags = EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED | CREATE_NO_WINDOW;
    if (restricted) {
        token = low_token();
        if (!token)
            goto failure;
        child->job = CreateJobObjectW(NULL, NULL);
        if (!child->job)
            goto failure;
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {0};
        limits.BasicLimitInformation.LimitFlags =
            JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_ACTIVE_PROCESS |
            JOB_OBJECT_LIMIT_PROCESS_TIME | JOB_OBJECT_LIMIT_JOB_MEMORY;
        limits.BasicLimitInformation.ActiveProcessLimit = 1;
        limits.BasicLimitInformation.PerProcessUserTimeLimit.QuadPart = 120000000;
        limits.JobMemoryLimit = 1024ULL * 1024 * 1024;
        JOBOBJECT_BASIC_UI_RESTRICTIONS ui = {JOB_OBJECT_UILIMIT_ALL};
        if (!SetInformationJobObject(child->job, JobObjectExtendedLimitInformation, &limits,
                                     sizeof(limits)) ||
            !SetInformationJobObject(child->job, JobObjectBasicUIRestrictions, &ui, sizeof(ui)))
            goto failure;
    }
    BOOL ok = restricted ? CreateProcessAsUserW(token, exe, command, NULL, NULL, TRUE, flags, NULL,
                                                directory, &start.StartupInfo, &process)
                         : CreateProcessW(exe, command, NULL, NULL, TRUE, flags, NULL, directory,
                                          &start.StartupInfo, &process);
    if (!ok)
        goto failure;
    child->process = process.hProcess;
    if (restricted && !AssignProcessToJobObject(child->job, child->process))
        goto failure;
    if (ResumeThread(process.hThread) == (DWORD)-1)
        goto failure;
    CloseHandle(process.hThread);
    process.hThread = NULL;
    DeleteProcThreadAttributeList(start.lpAttributeList);
    free(start.lpAttributeList);
    free(command);
    CloseHandle(source);
    CloseHandle(writes[0]);
    CloseHandle(writes[1]);
    if (token)
        CloseHandle(token);
    return 0;
failure:
    error = GetLastError();
    if (process.hThread)
        CloseHandle(process.hThread);
    if (start.lpAttributeList) {
        if (attributes_ready)
            DeleteProcThreadAttributeList(start.lpAttributeList);
        free(start.lpAttributeList);
    }
    free(command);
    if (source)
        CloseHandle(source);
    if (writes[0])
        CloseHandle(writes[0]);
    if (writes[1])
        CloseHandle(writes[1]);
    if (token)
        CloseHandle(token);
    cc_win_process_close(child, 1);
    int result = cc_win_errno(error);
    SetLastError(error);
    return result;
}
void cc_win_process_close(CCWinProcess *child, int terminate)
{
    if (terminate && child->process) {
        if (child->job)
            TerminateJobObject(child->job, 1);
        /* Assignment to the Job Object may itself have failed. Always stop the
         * suspended child as well instead of leaving an unassigned process. */
        TerminateProcess(child->process, 1);
        WaitForSingleObject(child->process, 5000);
    }
    if (child->process)
        CloseHandle(child->process);
    if (child->job)
        CloseHandle(child->job);
    if (child->output)
        CloseHandle(child->output);
    if (child->errors)
        CloseHandle(child->errors);
    memset(child, 0, sizeof(*child));
}
int cc_win_pipe_read(HANDLE pipe, void *buffer, size_t capacity, int *ended)
{
    DWORD available = 0, got = 0;
    if (!PeekNamedPipe(pipe, NULL, 0, NULL, &available, NULL)) {
        if (GetLastError() == ERROR_BROKEN_PIPE) {
            *ended = 1;
            return 0;
        }
        return cc_win_errno(GetLastError());
    }
    if (!available)
        return 0;
    if (capacity < available)
        available = (DWORD)capacity;
    if (!ReadFile(pipe, buffer, available, &got, NULL))
        return cc_win_errno(GetLastError());
    return (int)got;
}
