#include "platform/windows/diagnostics_windows.h"
#include "platform/windows/native_windows.h"
#include <stdio.h>
#include <string.h>
int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "--launch-detached")) {
        wchar_t exe[32768], command[32768];
        if (!MultiByteToWideChar(CP_UTF8, 0, argv[2], -1, exe, 32768))
            return 2;
        swprintf(command, 32768, L"\"%ls\" --version", exe);
        FreeConsole();
        STARTUPINFOW startup = {.cb = sizeof(startup), .dwFlags = STARTF_USESTDHANDLES};
        PROCESS_INFORMATION child;
        if (!CreateProcessW(exe, command, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &startup,
                            &child))
            return 2;
        if (WaitForSingleObject(child.hProcess, 20000) != WAIT_OBJECT_0) {
            TerminateProcess(child.hProcess, 2);
            WaitForSingleObject(child.hProcess, 2000);
        }
        DWORD code = 2;
        GetExitCodeProcess(child.hProcess, &code);
        CloseHandle(child.hThread);
        CloseHandle(child.hProcess);
        return (int)code;
    }
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    cc_win_diagnostics_start(0, 0);
    cc_win_startup_note("Deliberate exception in a disposable diagnostics fixture.");
    RaiseException(EXCEPTION_ACCESS_VIOLATION, EXCEPTION_NONCONTINUABLE, 0, NULL);
    return 1;
}
