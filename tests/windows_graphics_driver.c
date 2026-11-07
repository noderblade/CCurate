#include "platform/windows/diagnostics_windows.h"
#include "platform/windows/graphics_windows.h"
#undef NDEBUG
#include <assert.h>
#include <stdio.h>

void cc_win_startup_note(const char *message)
{
    fprintf(stderr, "[graphics-test] %s\n", message);
}
int main(void)
{
    RInitDesc init = {.backend = R_BACKEND_VULKAN,
                      .app_name = "CCurate graphics lifecycle test",
                      .worker_threads = 2};
    for (int cycle = 0; cycle < 4; cycle++) {
        assert(cc_win_graphics_init(&init));
        Rid window = Rwindow_create("CCurate software lifecycle test", 640, 360, R_WINDOW_DEFAULT);
        assert(window != R_INVALID_ID);
        Rwindow_set_content(window, Rlabel_create("Software Vulkan lifecycle test"));
        for (int frame = 0; frame < 8; frame++)
            assert(Rwait_frame(.02));
        cc_win_graphics_shutdown();
    }
    puts("PASS four software-device initialization / frame / shutdown cycles");
    return 0;
}
