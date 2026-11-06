/* Test harness for generated, trusted fixtures. The application itself always
 * uses the privilege-dropping worker. This executable is not installed. */
#include "curator.h"
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
int main(int argc, char **argv)
{
    if (argc == 5 && !strcmp(argv[1], "--preview-worker"))
        return preview_worker_main(atoi(argv[2]), atoi(argv[3]), atoi(argv[4]));
    int cancelled = argc > 1 && !strcmp(argv[1], "--cancel");
    atomic_int cancel = cancelled;
    int spawned = cancelled || (argc > 1 && !strcmp(argv[1], "--spawn"));
    if (spawned) {
        argv++;
        argc--;
    }
    if (argc < 2)
        return 2;
    int fd = open(argv[1], O_RDONLY);
    if (fd < 0)
        return 2;
    Preview p;
    int rc = spawned ? preview_spawn(fd, argc > 2 ? atoi(argv[2]) : 0, 0, &p, &cancel)
                     : preview_read(fd, argc > 2 ? atoi(argv[2]) : 0, 0, &p);
    if (p.error)
        rc = -1;
    close(fd);
    puts(p.text ? p.text : "");
    printf("pixels=%dx%d pages=%d\n", p.width, p.height, p.pages);
    if (argc > 3 && p.rgba) {
        FILE *f = fopen(argv[3], "wb");
        if (f) {
            fprintf(f, "P6\n%d %d\n255\n", p.width, p.height);
            for (int i = 0; i < p.width * p.height; i++)
                fwrite(p.rgba + 4 * i, 1, 3, f);
            fclose(f);
        }
    }
    preview_free(&p);
    return rc ? 1 : 0;
}
