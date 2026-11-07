#include "curator.h"
#include "platform/windows/process_windows.h"

typedef struct {
    uint32_t magic, width, height, pages, text_bytes, pixel_bytes, error;
} WinPacket;
static int send_preview(Preview *p)
{
    size_t length = strlen(p->text ? p->text : "");
    if (length > 900000)
        length = 900000;
    WinPacket packet = {0x52435032,       p->width,
                        p->height,        p->pages,
                        (uint32_t)length, (uint32_t)((size_t)p->width * p->height * 4),
                        p->error != 0};
    return write_all(1, &packet, sizeof(packet)) || write_all(1, p->text ? p->text : "", length) ||
           (packet.pixel_bytes && write_all(1, p->rgba, packet.pixel_bytes));
}
static int restricted_worker(void)
{
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits;
    DWORD required = JOB_OBJECT_LIMIT_JOB_MEMORY | JOB_OBJECT_LIMIT_ACTIVE_PROCESS |
                     JOB_OBJECT_LIMIT_PROCESS_TIME | JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!QueryInformationJobObject(NULL, JobObjectExtendedLimitInformation, &limits, sizeof(limits),
                                   NULL) ||
        (limits.BasicLimitInformation.LimitFlags & required) != required ||
        !limits.JobMemoryLimit || limits.JobMemoryLimit > 1024ULL * 1024 * 1024 ||
        limits.BasicLimitInformation.ActiveProcessLimit != 1 ||
        limits.BasicLimitInformation.PerProcessUserTimeLimit.QuadPart <= 0 ||
        limits.BasicLimitInformation.PerProcessUserTimeLimit.QuadPart > 120000000)
        return 0;
    JOBOBJECT_BASIC_UI_RESTRICTIONS ui;
    if (!QueryInformationJobObject(NULL, JobObjectBasicUIRestrictions, &ui, sizeof(ui), NULL) ||
        (ui.UIRestrictionsClass & JOB_OBJECT_UILIMIT_ALL) != JOB_OBJECT_UILIMIT_ALL)
        return 0;
    HANDLE token;
    DWORD size = 0;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return 0;
    GetTokenInformation(token, TokenIntegrityLevel, NULL, 0, &size);
    TOKEN_MANDATORY_LABEL *label = xmalloc(size);
    int ok = GetTokenInformation(token, TokenIntegrityLevel, label, size, &size);
    if (ok) {
        DWORD count = *GetSidSubAuthorityCount(label->Label.Sid);
        ok =
            count && *GetSidSubAuthority(label->Label.Sid, count - 1) <= SECURITY_MANDATORY_LOW_RID;
    }
    free(label);
    CloseHandle(token);
    return ok && !cc_win_elevated();
}
int preview_worker_main(int fd, int page, int hex_only)
{
    Preview p = {0};
    if (!restricted_worker()) {
        p.error = 1;
        p.text = xstrdup("Preview blocked: Windows worker restrictions are not active.");
    } else
        preview_read(fd, page, hex_only, &p);
    int rc = send_preview(&p);
    preview_free(&p);
    return rc;
}
int preview_spawn(int fd, int page, int hex_only, Preview *p, atomic_int *cancel)
{
    memset(p, 0, sizeof(*p));
    char pg[32], hex[16];
    snprintf(pg, sizeof(pg), "%d", page);
    snprintf(hex, sizeof(hex), "%d", hex_only);
    const char *arguments[] = {"--preview-worker", "0", pg, hex, NULL};
    CCWinProcess child;
    if (cc_win_spawn(&child, fd, arguments, 1)) {
        p->error = 1;
        p->text = fmt("Preview blocked: cannot start restricted Windows worker (error %lu).",
                      (unsigned long)GetLastError());
        return -1;
    }
    Buf bytes = {0};
    char diagnostics[32769] = {0}, reason[256] = {0};
    size_t diagnostic_size = 0;
    int ended[2] = {0}, cancelled = 0;
    int64_t start = monotonic_ns();
    while (!ended[0] || !ended[1]) {
        if (cancel && atomic_load(cancel)) {
            cancelled = 1;
            break;
        }
        if (monotonic_ns() - start > 20000000000LL) {
            strcpy(reason, "20-second preview timeout");
            break;
        }
        for (int i = 0; i < 2; i++) {
            if (ended[i])
                continue;
            unsigned char chunk[65536];
            int got =
                cc_win_pipe_read(i ? child.errors : child.output, chunk, sizeof(chunk), &ended[i]);
            if (got < 0) {
                snprintf(reason, sizeof(reason), "Preview pipe: %s", strerror(errno));
                break;
            }
            if (!got)
                continue;
            if (i) {
                size_t keep = (size_t)got;
                if (keep > sizeof(diagnostics) - 1 - diagnostic_size)
                    keep = sizeof(diagnostics) - 1 - diagnostic_size;
                memcpy(diagnostics + diagnostic_size, chunk, keep);
                diagnostic_size += keep;
            } else {
                if (bytes.n + (size_t)got > 5 * 1024 * 1024) {
                    strcpy(reason, "Preview output exceeded 5 MiB");
                    break;
                }
                if (bytes.n + (size_t)got > bytes.cap) {
                    bytes.cap = bytes.n + (size_t)got + 65536;
                    char *next = realloc(bytes.s, bytes.cap);
                    if (!next)
                        abort();
                    bytes.s = next;
                }
                memcpy(bytes.s + bytes.n, chunk, (size_t)got);
                bytes.n += (size_t)got;
            }
        }
        if (*reason)
            break;
        Sleep(10);
    }
    DWORD status = 0;
    if (!*reason && !cancelled) {
        if (WaitForSingleObject(child.process, 1000) != WAIT_OBJECT_0)
            strcpy(reason, "Worker did not exit after closing its output");
        GetExitCodeProcess(child.process, &status);
    }
    cc_win_process_close(&child, *reason || cancelled);
    if (cancelled) {
        bfree(&bytes);
        p->text = xstrdup("Preview cancelled by a newer selection.");
        return 1;
    }
    if (!*reason && bytes.n >= sizeof(WinPacket)) {
        WinPacket packet;
        memcpy(&packet, bytes.s, sizeof(packet));
        if (packet.magic == 0x52435032 && packet.width <= 1000 && packet.height <= 800 &&
            packet.pixel_bytes == packet.width * packet.height * 4 &&
            packet.text_bytes < 1024 * 1024 &&
            bytes.n == sizeof(packet) + packet.text_bytes + packet.pixel_bytes) {
            p->width = packet.width;
            p->height = packet.height;
            p->pages = packet.pages;
            p->error = packet.error != 0;
            p->text = xmalloc((size_t)packet.text_bytes + 1);
            memcpy(p->text, bytes.s + sizeof(packet), packet.text_bytes);
            if (packet.pixel_bytes) {
                p->rgba = xmalloc(packet.pixel_bytes);
                memcpy(p->rgba, bytes.s + sizeof(packet) + packet.text_bytes, packet.pixel_bytes);
            }
            if (p->error && *diagnostics) {
                char *safe = display_path(diagnostics),
                     *message = fmt("%s\nDecoder diagnostics: %s", p->text, safe);
                free(safe);
                free(p->text);
                p->text = message;
            }
            bfree(&bytes);
            return p->error ? -1 : 0;
        }
        strcpy(reason, "Invalid preview packet");
    } else if (!*reason)
        snprintf(reason, sizeof(reason), "Worker exited without a result (Windows status 0x%08lx)",
                 (unsigned long)status);
    bfree(&bytes);
    p->error = 1;
    char *safe = display_path(diagnostics);
    p->text =
        fmt("Preview unavailable: %s.%s%s", reason, *safe ? "\nDecoder diagnostics: " : "", safe);
    free(safe);
    return -1;
}
