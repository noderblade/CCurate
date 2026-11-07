#include "curator.h"
#include "desktop_open.h"
#include "platform/windows/process_windows.h"
#include <shellapi.h>

int desktop_open_start(DesktopOpen *job, int root, const char *relative)
{
    if (job->pid > 0)
        return -1;
    memset(job, 0, sizeof(*job));
    job->fd = -1;
    if (cc_win_elevated()) {
        snprintf(job->message, sizeof(job->message),
                 "Default application opening is disabled while elevated. Run CCurate as a normal "
                 "desktop user to open external applications.");
        return -1;
    }
    CCWinProcess *child = xmalloc(sizeof(*child));
    const char *arguments[] = {"--open-worker", "0", "1", relative, NULL};
    if (cc_win_spawn(child, root, arguments, 0)) {
        snprintf(job->message, sizeof(job->message),
                 "Cannot start desktop launcher (Windows error %lu).",
                 (unsigned long)GetLastError());
        free(child);
        return -1;
    }
    job->native = child;
    job->pid = 1;
    job->started = (double)monotonic_ns() / 1e9;
    return 0;
}
int desktop_open_poll(DesktopOpen *job)
{
    if (!job->pid)
        return 0;
    CCWinProcess *child = job->native;
    int ended = 0;
    int n = cc_win_pipe_read(child->output, job->message + job->used,
                             sizeof(job->message) - 1 - job->used, &ended);
    if (n > 0)
        job->used += (size_t)n;
    int timeout = (double)monotonic_ns() / 1e9 - job->started > 15;
    if (n >= 0 && !timeout && WaitForSingleObject(child->process, 0) != WAIT_OBJECT_0)
        return 0;
    if (n < 0 || timeout) {
        snprintf(job->message, sizeof(job->message),
                 timeout ? "Desktop launcher timed out." : "Desktop launcher pipe failed.");
    } else {
        do {
            n = cc_win_pipe_read(child->output, job->message + job->used,
                                 sizeof(job->message) - 1 - job->used, &ended);
            if (n > 0)
                job->used += (size_t)n;
        } while (n > 0 && job->used < sizeof(job->message) - 1);
    }
    DWORD status = 1;
    GetExitCodeProcess(child->process, &status);
    int ok = !timeout && status == 0 && !strncmp(job->message, "OK", 2);
    if (!ok && !*job->message)
        snprintf(job->message, sizeof(job->message), "The desktop did not accept the file.");
    cc_win_process_close(child, timeout || n < 0);
    free(child);
    job->native = NULL;
    job->pid = 0;
    return ok ? 1 : -1;
}
void desktop_open_stop(DesktopOpen *job)
{
    if (job->native) {
        cc_win_process_close(job->native, 1);
        free(job->native);
    }
    job->native = NULL;
    job->pid = 0;
}
int desktop_open_worker(int root, int result_fd, const char *relative)
{
    char error[512] = {0};
    if (cc_win_elevated())
        strcpy(error, "Refusing to launch an external application while elevated.");
    int fd = !*error ? open_beneath(root, relative, O_RDONLY, 0) : -1;
    struct stat st;
    wchar_t *path = NULL;
    if (!*error && (fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode)))
        snprintf(error, sizeof(error), "Cannot open a regular file: %s", strerror(errno));
    if (!*error)
        path = cc_win_handle_path(cc_win_handle(fd));
    if (fd >= 0)
        close(fd);
    if (!*error && !path)
        strcpy(error, "Cannot resolve the file for the desktop.");
    if (!*error) {
        HRESULT initialized = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
        SHELLEXECUTEINFOW info = {0};
        info.cbSize = sizeof(info);
        info.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
        info.lpVerb = L"open";
        info.lpFile = path;
        info.nShow = SW_SHOWNORMAL;
        if (!ShellExecuteExW(&info))
            snprintf(error, sizeof(error), "Default application launch failed (Windows error %lu).",
                     (unsigned long)GetLastError());
        if (SUCCEEDED(initialized))
            CoUninitialize();
    }
    free(path);
    if (*error) {
        write_all(result_fd, error, strlen(error));
        return 1;
    }
    write_all(result_fd, "OK", 2);
    return 0;
}
