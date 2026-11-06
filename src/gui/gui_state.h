#ifndef CCURATE_GUI_STATE_H
#define CCURATE_GUI_STATE_H

/* Private GUI state, owned by gui_main until its workers and callbacks stop.
 * Widgets, navigation and settings belong to the UI thread. Preview request/result
 * fields are protected by pm/pc; operation completion and cancellation use atomics.
 * Components receive Gui explicitly; no mutable GUI singleton is exported. */
#include "curator.h"
#include "desktop_open.h"
#include "font_catalog.h"
#include "gui_scroll.h"
#include "gui_types.h"
#include <Raven.h>

typedef struct GuiLocation GuiLocation;
struct GuiPane {
    char *path, *search;
    char *anchor;
    GuiLocation *locations;
    Entry *rows;
    unsigned char *marked;
    int n, selected, offset, page, page_end, sort, created;
    int horizontal, name_chars, scroll_pending, scroll_row;
    int64_t total;
    char **marks;
    int mark_count;
};
struct Gui {
    Catalog *c, reader;
    Rid window, canvas, overlay, prompt_box, prompt_label, input, font, texture;
    FontService *font_service;
    MonoFonts *fonts;
    char *font_file, *font_family, *font_style;
    int font_index, font_dialog, font_candidate, font_scroll, font_control, font_previous_focus;
    int *font_matches, font_match_count, font_match_row;
    int font_load_row, font_load_mode, font_apply_pending;
    uint64_t font_serial;
    Rid font_sample;
    char font_filter[128], font_error[256];
    GuiPane pane[2];
    DesktopOpen opener;
    char *open_path;
    int focus, history, filter, hex, page, dialog, preview_scroll, details_scroll, help;
    int preview_horizontal, details_horizontal, font_horizontal;
    GuiScrollbar scrollbars[GUI_SCROLLBARS];
    GuiTextCache text_cache[2];
    int scroll_drag;
    float scroll_grab;
    float width, height, row_height, font_size, scale;
    float split_top, split_bottom, split_rows;
    int drag, splitter_hover, menu, menu_row, menu_offset, footer_hover;
    ViewService *view;
    uint64_t view_serial;
    unsigned view_dirty;
    int target_refresh, occurrence_page, filter_opener;
    ScanStatus scan;
    char *details, *message, *prompt_source;
    Preview preview;
    int64_t preview_version;
    char *preview_path;
    pthread_t worker;
    atomic_int busy, finished;
    char *job, *arg1, *arg2, **selections;
    int selection_count, complete, job_result;
    pthread_t preview_thread;
    pthread_mutex_t pm;
    pthread_cond_t pc;
    int preview_stop, preview_root, preview_page, preview_hex, pending_seq, request_seq, result_seq;
    char *preview_file;
    atomic_int preview_cancel;
    Preview ready;
    int ready_available;
    int log_visible, log_level, log_follow, log_scroll, log_horizontal, log_count, theme;
    int dropdown, dropdown_row, dropdown_offset, sort_side;
    float split_log;
    ActivityLine *log_lines;
    uint64_t log_revision;
    char log_error[256];
    double last_click;
    float click_x, click_y;
    int click_count;
    Summary summary;
};

#endif /* CCURATE_GUI_STATE_H */
