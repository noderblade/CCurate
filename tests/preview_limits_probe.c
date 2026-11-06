/* Test-only interposer, never installed or loaded by the application itself.
 * Exercise real worker limits at the same point an image decoder starts. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static void require(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "Preview policy probe failed: %s (errno %d)\n", message, errno);
        _exit(91);
    }
}
void *gdk_pixbuf_loader_new(void)
{
    const char *mode = getenv("CCURATE_TEST_PREVIEW_POLICY");
    if (mode) {
        int fd = memfd_create("curator-decoder-probe", MFD_CLOEXEC);
        require(fd >= 0, "memfd_create");
        if (!strcmp(mode, "oversize")) {
            /* Sparse allocation: no 513 MiB payload is generated. */
            require(!ftruncate(fd, 513LL * 1024 * 1024), "oversize memfd unexpectedly returned");
            _exit(92); /* The worker must instead be stopped by SIGXFSZ. */
        }
        require(!ftruncate(fd, 48LL * 1024 * 1024), "48 MiB decoded-pixel memfd");
        require(pwrite(fd, "pixels", 6, 0) == 6, "memfd write");
        close(fd);
        FILE *scratch = tmpfile();
        require(scratch != NULL, "temporary input file");
        require(fwrite("compressed image input", 1, 22, scratch) == 22, "temporary file write");
        require(!fflush(scratch), "temporary file flush");
        fclose(scratch);
        require((fcntl(3, F_GETFL) & O_ACCMODE) == O_RDONLY, "source descriptor is read-only");
        errno = 0;
        require(write(3, "x", 1) == -1 && errno == EBADF, "source write refused");
        errno = 0;
        require(ftruncate(3, 0) == -1 && (errno == EBADF || errno == EINVAL),
                "source truncate refused");
        int families[] = {AF_INET, AF_INET6};
        for (int i = 0; i < 2; i++) {
            errno = 0;
            fd = socket(families[i], SOCK_STREAM | SOCK_CLOEXEC, 0);
            require(fd < 0 && errno == EPERM, "Internet socket creation refused");
        }
        int pair[2];
        require(!socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair), "local decoder IPC");
        struct sockaddr_un address = {.sun_family = AF_UNIX};
        strcpy(address.sun_path, "/nonexistent-curator-policy-probe");
        errno = 0;
        require(connect(pair[0], (struct sockaddr *)&address, sizeof(address)) == -1 &&
                    errno == EPERM,
                "connect remains blocked");
        close(pair[0]);
        close(pair[1]);
    }
    void *(*original)(void) = dlsym(RTLD_NEXT, "gdk_pixbuf_loader_new");
    require(original != NULL, "system image decoder symbol");
    return original();
}
