#include "curator.h"
#ifdef CCURATE_GUI
#include "desktop_open.h"
#include "gui.h"
#endif
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static Catalog *active;
static void stop(int sig)
{
    (void)sig;
    if (active)
        atomic_store(&active->cancel, 1);
}
static void usage(void)
{
    puts("CCurate 0.1.0\n\n"
         "GUI:     ccurate [SOURCE [DESTINATION]]\n"
         "         Missing folders are chosen in a graphical browser.\n"
         "Browse:  ccurate --browse [DESTINATION]\n"
         "CLI:     ccurate --cli SOURCE DESTINATION COMMAND [ARG...]\n"
         "Commands:\n"
         "  scan [--full]                 refresh live source (SHA-512)\n"
         "  copy SOURCE_REL TARGET_DIR [--complete]\n"
         "  review|unreview|reject|restore SOURCE_REL\n"
         "  mkdir TARGET_REL             create destination directory\n"
         "  move OLD_TARGET_REL NEW_TARGET_REL\n"
         "  list [SOURCE_DIR] [--history] [--unsecured]\n"
         "  info SOURCE_REL              locations / duplicates / versions\n"
         "  audit | reconcile | balance | export\n"
         "  import MANIFEST.jsonl        rebuild an EMPTY catalog\n"
         "Use '-' as SOURCE for offline archive operations.\n"
         "Paths passed to commands are relative; empty directory is ''.\n"
         "Reject records a decision. It never deletes source data.\n");
}
int main(int argc, char **argv)
{
#ifdef CCURATE_GUI
    if (argc > 1 && !strcmp(argv[1], "--open-worker")) {
        if (argc != 5)
            return 2;
        return desktop_open_worker(atoi(argv[2]), atoi(argv[3]), argv[4]);
    }
#endif
    if (argc > 1 && !strcmp(argv[1], "--preview-worker")) {
        if (argc != 5)
            return 2;
        return preview_worker_main(atoi(argv[2]), atoi(argv[3]), atoi(argv[4]));
    }
    if (argc > 1 && !strcmp(argv[1], "--help")) {
        usage();
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "--version")) {
        puts("CCurate 0.1.0 / Raven ede33eb15f986766f063aadb109f3c379cd8f6ea");
        return 0;
    }
    int cli = argc > 1 && !strcmp(argv[1], "--cli"),
        browse = argc > 1 && !strcmp(argv[1], "--browse"), choose = 0;
    const char *src = NULL, *dst = NULL, *cmd = NULL;
    int arg = 0;
    if (cli) {
        if (argc < 5) {
            usage();
            return 2;
        }
        src = strcmp(argv[2], "-") ? argv[2] : NULL;
        dst = argv[3];
        cmd = argv[4];
        arg = 5;
        browse = !src;
    } else if (browse) {
        if (argc > 3) {
            usage();
            return 2;
        }
        dst = argc == 3 ? argv[2] : NULL;
        choose = !dst;
    } else {
        if (argc > 3 || (argc > 1 && argv[1][0] == '-')) {
            usage();
            return 2;
        }
        src = argc > 1 ? argv[1] : NULL;
        dst = argc > 2 ? argv[2] : NULL;
        choose = !dst;
    }
#ifdef CCURATE_GUI
    char *selected_source = choose && src ? xstrdup(src) : NULL;
    char *selected_destination = NULL, *startup_error = NULL;
#else
    if (choose) {
        usage();
        fputs("Folder selection requires a build with CCURATE_GUI=ON.\n", stderr);
        return 2;
    }
#endif
    Catalog c;
    for (;;) {
#ifdef CCURATE_GUI
        if (choose) {
            int result =
                gui_choose_roots(&selected_source, &selected_destination, browse, startup_error);
            free(startup_error);
            startup_error = NULL;
            if (result <= 0) {
                free(selected_source);
                free(selected_destination);
                return result < 0 ? 1 : 0;
            }
            src = selected_source;
            dst = selected_destination;
        }
#endif
        if (!catalog_open(&c, src, dst, browse))
            break;
        fprintf(stderr, "%s\n", c.error);
#ifdef CCURATE_GUI
        if (choose)
            startup_error = xstrdup(c.error);
#endif
        catalog_close(&c);
        if (!choose)
            return 1;
    }
#ifdef CCURATE_GUI
    free(selected_source);
    free(selected_destination);
#endif
    active = &c;
    signal(SIGINT, stop);
    signal(SIGTERM, stop);
    int rc = 0;
    if (!cli) {
#ifdef CCURATE_GUI
        rc = gui_main(&c, browse);
#else
        fputs("Built without Raven GUI; rebuild with CCURATE_GUI=ON\n", stderr);
        rc = 1;
#endif
    } else if (!strcmp(cmd, "scan"))
        rc = scan_source(&c, argc > arg && !strcmp(argv[arg], "--full"));
    else if (!strcmp(cmd, "copy") && argc >= arg + 2)
        rc = copy_entry(&c, argv[arg], argv[arg + 1],
                        argc > arg + 2 && !strcmp(argv[arg + 2], "--complete"));
    else if ((!strcmp(cmd, "review") || !strcmp(cmd, "unreview") || !strcmp(cmd, "reject") ||
              !strcmp(cmd, "restore")) &&
             argc > arg)
        rc = mark_entries(&c, argv[arg], cmd);
    else if (!strcmp(cmd, "move") && argc >= arg + 2)
        rc = move_target(&c, argv[arg], argv[arg + 1]);
    else if (!strcmp(cmd, "mkdir") && argc > arg) {
        if (!valid_relative(argv[arg], 0) || descendant(argv[arg], META_DIR))
            rc = fail(&c, "Invalid directory");
        else
            rc = mkdir_beneath(c.dfd, argv[arg]);
    } else if (!strcmp(cmd, "audit"))
        rc = audit_target(&c);
    else if (!strcmp(cmd, "reconcile"))
        rc = reconcile_target(&c);
    else if (!strcmp(cmd, "export"))
        rc = export_archive(&c);
    else if (!strcmp(cmd, "import") && argc > arg)
        rc = import_archive(&c, argv[arg]);
    else if (!strcmp(cmd, "balance")) {
        char *s = summary_text(&c);
        puts(s);
        free(s);
    } else if (!strcmp(cmd, "info") && argc > arg) {
        Entry e = {0};
        rc = load_entry(&c, argv[arg], &e, 0);
        if (!rc) {
            char *s = entry_details(&c, &e);
            puts(s);
            free(s);
            entry_free(&e);
        }
    } else if (!strcmp(cmd, "list")) {
        int hist = 0, filter = 0;
        const char *path = "";
        for (int i = arg; i < argc; i++) {
            if (!strcmp(argv[i], "--history"))
                hist = 1;
            else if (!strcmp(argv[i], "--unsecured"))
                filter = 1;
            else
                path = argv[i];
        }
        Entry *e = NULL;
        int n = list_entries(&c, path, &e, hist, filter, NULL);
        for (int i = 0; i < n; i++) {
            char *p = display_path(e[i].path);
            printf("%s\t%lld\t%s\t%s\t%s\n", e[i].kind, (long long)e[i].size,
                   e[i].discarded ? "rejected"
                   : *e[i].state  ? e[i].state
                                  : "pending",
                   p, e[i].dest);
            free(p);
        }
        entries_free(e, n);
    } else {
        usage();
        rc = 2;
    }
    if (rc)
        fprintf(stderr, "%s\n", *c.error ? c.error : "Operation incomplete; consult audit");
    catalog_close(&c);
    active = NULL;
    return rc < 0 ? 1 : rc;
}
