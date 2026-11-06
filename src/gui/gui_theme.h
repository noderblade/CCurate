#ifndef CCURATE_GUI_THEME_H
#define CCURATE_GUI_THEME_H

/* Immutable palettes, theme application and file-status legend. */
#include "gui_types.h"

typedef struct {
    const char *name;
    int dark;
    unsigned background, panel, raised, border, text, muted, accent, selection, inactive, alternate,
        success, warning, error, duplicate, directory, dim;
} GuiTheme;

#define GUI_HEX_COLOR(v)                                                                           \
    (((v) >> 16) & 255) / 255.f, (((v) >> 8) & 255) / 255.f, ((v) & 255) / 255.f
#define GUI_COLOR(ui, role) GUI_HEX_COLOR(gui_theme_current(ui)->role)

void gui_apply_theme(Gui *ui, int index);
int gui_theme_count(void);
const GuiTheme *gui_theme_get(int index);
const GuiTheme *gui_theme_current(const Gui *ui);
const char *gui_theme_legend(void);

#endif /* CCURATE_GUI_THEME_H */
