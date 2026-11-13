#include "usage_driver_windows.h"
#include "curator.h"
#include "platform/windows/native_windows.h"
#include <stdlib.h>

int wmain(int argc, wchar_t **wide);

/* Test the UTF-8 service with the same argument semantics as the application.
 * The Windows narrow CRT main() would lose non-ANSI source paths. */
int wmain(int argc, wchar_t **wide)
{
    char **argv = xmalloc(((size_t)argc + 1) * sizeof(*argv));
    int count = 0, result = 2;
    for (; count < argc; count++) {
        argv[count] = cc_win_utf8(wide[count]);
        if (!argv[count])
            break;
    }
    if (count == argc)
        result = cc_usagecheck_main(argc, argv);
    for (int i = 0; i < count; i++)
        free(argv[i]);
    free(argv);
    return result;
}
