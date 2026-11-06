#include "desktop_open.h"
#include "curator.h"
#include <errno.h>
#include <fcntl.h>
#include <gio/gio.h>
#include <grp.h>
#include <limits.h>
#include <pwd.h>
#include <signal.h>
#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

static double monotonic(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec + now.tv_nsec / 1e9;
}

int desktop_open_start(DesktopOpen *job, int root, const char *relative)
{
    if (job->pid > 0)
        return -1;
    memset(job, 0, sizeof(*job));
    job->fd = -1;
    int pipefd[2] = {-1, -1}, root_copy = -1, output_copy = -1;
    if (root < 0 || !valid_relative(relative, 0)) {
        snprintf(job->message, sizeof(job->message), "This file is not available.");
        return -1;
    }
    if (pipe2(pipefd, O_CLOEXEC) || (root_copy = fcntl(root, F_DUPFD_CLOEXEC, 10)) < 0 ||
        (output_copy = fcntl(pipefd[1], F_DUPFD_CLOEXEC, 10)) < 0)
        goto failed;
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, output_copy, 3);
    posix_spawn_file_actions_adddup2(&actions, root_copy, 4);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    char *args[] = {"ccurate", "--open-worker", "4", "3", (char *)relative, NULL};
    int rc = posix_spawn(&job->pid, "/proc/self/exe", &actions, NULL, args, environ);
    posix_spawn_file_actions_destroy(&actions);
    if (rc) {
        errno = rc;
        goto failed;
    }
    close(root_copy);
    close(output_copy);
    close(pipefd[1]);
    job->fd = pipefd[0];
    fcntl(job->fd, F_SETFL, fcntl(job->fd, F_GETFL) | O_NONBLOCK);
    job->started = monotonic();
    return 0;
failed:
    snprintf(job->message, sizeof(job->message), "Cannot start desktop launcher: %s",
             strerror(errno));
    if (root_copy >= 0)
        close(root_copy);
    if (output_copy >= 0)
        close(output_copy);
    if (pipefd[0] >= 0)
        close(pipefd[0]);
    if (pipefd[1] >= 0)
        close(pipefd[1]);
    job->pid = 0;
    return -1;
}

int desktop_open_poll(DesktopOpen *job)
{
    if (job->pid <= 0)
        return 0;
    ssize_t n;
    while (job->used < sizeof(job->message) - 1 &&
           (n = read(job->fd, job->message + job->used, sizeof(job->message) - 1 - job->used)) > 0)
        job->used += n;
    job->message[job->used] = 0;
    int status;
    pid_t done = waitpid(job->pid, &status, WNOHANG);
    if (done == 0 && monotonic() - job->started < 30)
        return 0;
    if (done == 0) {
        desktop_open_stop(job);
        snprintf(job->message, sizeof(job->message),
                 "The desktop launcher did not respond within 30 seconds.");
        return -1;
    }
    /* Drain once more after exit, including diagnostics written after the first read. */
    while (job->used < sizeof(job->message) - 1 &&
           (n = read(job->fd, job->message + job->used, sizeof(job->message) - 1 - job->used)) > 0)
        job->used += n;
    job->message[job->used] = 0;
    close(job->fd);
    job->fd = -1;
    job->pid = 0;
    if (done < 0 || !WIFEXITED(status) || WEXITSTATUS(status)) {
        if (!*job->message)
            snprintf(job->message, sizeof(job->message), "The desktop launcher failed.");
        return -1;
    }
    return 1;
}

void desktop_open_stop(DesktopOpen *job)
{
    if (job->pid > 0) {
        /* Only stop the launcher; never signal an already opened application. */
        kill(job->pid, SIGKILL);
        while (waitpid(job->pid, NULL, 0) < 0 && errno == EINTR) {
        }
        close(job->fd);
        job->pid = 0;
        job->fd = -1;
    }
}

static int desktop_user(int result_fd)
{
    if (geteuid() != 0)
        return 0;
    const char *id = getenv("SUDO_UID");
    if (!id || !*id)
        id = getenv("PKEXEC_UID");
    char *end = NULL;
    errno = 0;
    unsigned long value = id && *id ? strtoul(id, &end, 10) : 0;
    struct passwd *user =
        value && value < UINT_MAX && !errno && end && !*end ? getpwuid((uid_t)value) : NULL;
    if (!user || user->pw_uid == 0) {
        dprintf(result_fd,
                "Cannot identify the desktop user. Start CCurate from your desktop account (sudo "
                "preserves SUDO_UID). External applications are never launched as root.");
        return -1;
    }
    char runtime[64], bus[96];
    snprintf(runtime, sizeof(runtime), "/run/user/%u", (unsigned)user->pw_uid);
    snprintf(bus, sizeof(bus), "unix:path=%s/bus", runtime);
    if (initgroups(user->pw_name, user->pw_gid) || setgid(user->pw_gid) || setuid(user->pw_uid)) {
        dprintf(result_fd, "Cannot switch to the desktop user: %s", strerror(errno));
        return -1;
    }
    setenv("HOME", user->pw_dir, 1);
    setenv("USER", user->pw_name, 1);
    setenv("LOGNAME", user->pw_name, 1);
    setenv("XDG_RUNTIME_DIR", runtime, 1);
    setenv("DBUS_SESSION_BUS_ADDRESS", bus, 1);
    /* Root's private defaults must not replace the desktop user's associations. */
    unsetenv("XDG_CONFIG_HOME");
    unsetenv("XDG_DATA_HOME");
    unsetenv("XDG_CACHE_HOME");
    return 0;
}

typedef struct {
    GMainLoop *loop;
    int fd, failed;
} Launch;

static void launched(GObject *object, GAsyncResult *result, gpointer data)
{
    (void)object;
    Launch *launch = data;
    GError *error = NULL;
    if (!g_app_info_launch_default_for_uri_finish(result, &error)) {
        launch->failed = 1;
        dprintf(launch->fd, "Cannot open in the default application: %s",
                error ? error->message : "no handler");
        g_clear_error(&error);
    }
    g_main_loop_quit(launch->loop);
}

int desktop_open_worker(int root, int result_fd, const char *relative)
{
    fcntl(result_fd, F_SETFD, FD_CLOEXEC);
    fcntl(root, F_SETFD, FD_CLOEXEC);
    int fd = open_beneath(root, relative, O_RDONLY | O_NONBLOCK, 0);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode)) {
        dprintf(result_fd, "Cannot open this regular file: %s",
                fd < 0 ? strerror(errno) : "unsupported file type");
        if (fd >= 0)
            close(fd);
        close(root);
        return 1;
    }
    char link[64], path[PATH_MAX + 1];
    snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
    ssize_t length = readlink(link, path, sizeof(path) - 1);
    close(fd);
    close(root);
    if (length <= 0 || length >= (ssize_t)sizeof(path) - 1) {
        dprintf(result_fd, "Cannot resolve the selected file's absolute path.");
        return 1;
    }
    path[length] = 0;
    if (desktop_user(result_fd))
        return 1;
    if (access(path, R_OK)) {
        dprintf(result_fd, "The desktop user cannot read this file: %s", strerror(errno));
        return 1;
    }
    GError *error = NULL;
    char *uri = g_filename_to_uri(path, NULL, &error);
    if (!uri) {
        dprintf(result_fd, "Cannot form the file URI: %s", error ? error->message : "invalid path");
        g_clear_error(&error);
        return 1;
    }
    Launch launch = {.loop = g_main_loop_new(NULL, FALSE), .fd = result_fd};
    g_app_info_launch_default_for_uri_async(uri, NULL, NULL, launched, &launch);
    g_main_loop_run(launch.loop);
    g_main_loop_unref(launch.loop);
    g_free(uri);
    return launch.failed;
}
