#include "gui_theme.h"
#include "gui_state.h"
#include <stdlib.h>
#include <string.h>

/* Immutable palettes, theme application and file-status legend. */
static const GuiTheme themes[] = {
    {"CCurate Dark", 1, 0x09131f, 0x111825, 0x1a2b3d, 0x34495d, 0xdce5f0, 0x9fb1c4, 0x52b9e8,
     0x203c50, 0x263343, 0x17202d, 0x63d7a8, 0xffd166, 0xff7b7b, 0xc7a4ed, 0x82b5ed, 0x8391a3},
    {"Tango Dark", 1, 0x202627, 0x2e3436, 0x414747, 0x555753, 0xeeeeec, 0xbabdb6, 0x729fcf,
     0x354960, 0x454b4d, 0x333a3c, 0x8ae234, 0xfce94f, 0xef6a6a, 0xad7fa8, 0x729fcf, 0x9a9d96},
    {"Tango Light", 0, 0xeeeeec, 0xffffff, 0xe3e5de, 0xbabdb6, 0x2e3436, 0x555753, 0x3465a4,
     0xd6e4f1, 0xe1e4e5, 0xf3f4ef, 0x3b7805, 0x8f5902, 0xa40000, 0x75507b, 0x204a87, 0x777a75},
    {"Solarized Dark", 1, 0x002b36, 0x073642, 0x164854, 0x586e75, 0x93a1a1, 0x839496, 0x268bd2,
     0x164858, 0x12414b, 0x0a3c48, 0x9eae32, 0xd0ab35, 0xef6661, 0xa29ed6, 0x49b7ae, 0x788e96},
    {"Solarized Light", 0, 0xfdf6e3, 0xeee8d5, 0xe4decc, 0x93a1a1, 0x586e75, 0x657b83, 0x268bd2,
     0xd7e1dd, 0xe1ded1, 0xf7f0dc, 0x5c7100, 0x8d6900, 0xc32624, 0x6c71c4, 0x147e78, 0x7c8583},
    {"Nord", 1, 0x2e3440, 0x3b4252, 0x434c5e, 0x616e88, 0xeceff4, 0xd8dee9, 0x88c0d0, 0x4c566a,
     0x434c5e, 0x3f4758, 0xa3be8c, 0xebcb8b, 0xe38c96, 0xb48ead, 0x81a1c1, 0xa2adbf},
    {"Dracula", 1, 0x21222c, 0x282a36, 0x343746, 0x6272a4, 0xf8f8f2, 0xb5b9d3, 0xbd93f9, 0x44475a,
     0x3b3e50, 0x2d303e, 0x50fa7b, 0xf1fa8c, 0xff7070, 0xff79c6, 0x8be9fd, 0x949dbb},
    {"Gruvbox Dark", 1, 0x1d2021, 0x282828, 0x3c3836, 0x665c54, 0xebdbb2, 0xbdae93, 0xfabd2f,
     0x504945, 0x3c3836, 0x32302f, 0xb8bb26, 0xfabd2f, 0xfb6556, 0xd3869b, 0x83a598, 0xa89984},
    {"Catppuccin Mocha", 1, 0x181825, 0x1e1e2e, 0x313244, 0x585b70, 0xcdd6f4, 0xa6adc8, 0xcba6f7,
     0x45475a, 0x313244, 0x252537, 0xa6e3a1, 0xf9e2af, 0xf38ba8, 0xf5c2e7, 0x89b4fa, 0x9399b2},
    {"Tokyo Night", 1, 0x16161e, 0x1a1b26, 0x24283b, 0x565f89, 0xc0caf5, 0xa9b1d6, 0x7aa2f7,
     0x283457, 0x24283b, 0x202232, 0x9ece6a, 0xe0af68, 0xf7768e, 0xbb9af7, 0x7dcfff, 0x8992b4}};

static const char *legend_text =
    "FILE STATUS LEGEND\n\n"
    "PENDING     Name and metadata found; scan verification is still pending.\n"
    "UNCOPIED    No verified destination copy has been secured yet.\n"
    "OK          File content has a SHA-512-verified destination copy.\n"
    "            For a non-file object, its metadata has been retained.\n"
    "O xN        First representative of N current identical-content paths.\n"
    "D xN        Another occurrence of the same content (a duplicate).\n"
    "OK O/D xN   That duplicate group has verified destination content.\n"
    "-- O/D xN   That duplicate group still needs verified destination content.\n"
    "REJECTED    Intentionally excluded; the source file is NOT deleted.\n"
    "ERROR       A scan/read operation failed. See details and Activity.\n"
    "CHANGED     A destination audit found a content mismatch/read failure.\n"
    "MISSING     An indexed destination copy was not found/accessed.\n"
    "UNCHECKED   Destination mapping needs verification; run Audit.\n"
    "SESSION     The internal .ccurate directory on the destination.\n\n"
    "DIRECTORIES\n"
    "OK s/n      Retained directory with s of n eligible descendants secured.\n"
    "-- s/n      Directory retention or descendant coverage is incomplete.\n"
    "REJ s/n     The directory is rejected.\n"
    "scanning... Completeness counts are deferred while a worker is active.\n"
    "            A pending directory may show PENDING instead.\n\n"
    "NAME PREFIXES AND COLORS\n"
    "r           Reviewed by you; independent of copied/rejected status.\n"
    "*           Selected for an operation; Ctrl+click / Space toggles it.\n"
    "Blue        Directory; green: secured content; purple: duplicate.\n"
    "Red         Error/changed/missing content; muted: rejected entry.\n"
    "Selection   Highlight identifies keyboard/click focus, not retention.\n"
    "            Palettes adapt the shades. Status text is authoritative.\n\n"
    "A verified status describes the last verification, not continuous monitoring.\n"
    "Reviewing a file does not secure it. Rejecting it never deletes source data.\n";

void gui_apply_theme(Gui *ui, int index)
{
    ui->theme = index >= 0 && index < gui_theme_count() ? index : 1;
    Rtheme_set_dark(gui_theme_current(ui)->dark);
    Rtheme_set_accent(GUI_COLOR(ui, accent));
    Rtheme_set_color(R_COLOR_WINDOW, GUI_COLOR(ui, background), 1);
    Rtheme_set_color(R_COLOR_PANEL, GUI_COLOR(ui, panel), 1);
    Rtheme_set_color(R_COLOR_TEXT, GUI_COLOR(ui, text), 1);
    Rtheme_set_color(R_COLOR_TEXT_DISABLED, GUI_COLOR(ui, dim), 1);
    Rtheme_set_color(R_COLOR_TEXT_PLACEHOLDER, GUI_COLOR(ui, muted), 1);
    Rtheme_set_color(R_COLOR_CONTROL, GUI_COLOR(ui, raised), 1);
    Rtheme_set_color(R_COLOR_CONTROL_HOVERED, GUI_COLOR(ui, selection), 1);
    Rtheme_set_color(R_COLOR_CONTROL_PRESSED, GUI_COLOR(ui, inactive), 1);
    Rtheme_set_color(R_COLOR_CONTROL_BORDER, GUI_COLOR(ui, border), 1);
    Rtheme_set_color(R_COLOR_INPUT, GUI_COLOR(ui, panel), 1);
    Rtheme_set_color(R_COLOR_FOCUS, GUI_COLOR(ui, accent), 1);
    Rtheme_set_color(R_COLOR_SELECTION, GUI_COLOR(ui, selection), 1);
    Rtheme_set_color(R_COLOR_POPUP, GUI_COLOR(ui, raised), 1);
    Rtheme_set_color(R_COLOR_POPUP_BORDER, GUI_COLOR(ui, border), 1);
    Rtheme_set_color(R_COLOR_TOOLTIP, GUI_COLOR(ui, raised), 1);
    Rtheme_set_color(R_COLOR_TOOLTIP_TEXT, GUI_COLOR(ui, text), 1);
    Rtheme_set_color(R_COLOR_SUCCESS, GUI_COLOR(ui, success), 1);
    Rtheme_set_color(R_COLOR_WARNING, GUI_COLOR(ui, warning), 1);
    Rtheme_set_color(R_COLOR_DANGER, GUI_COLOR(ui, error), 1);
    Rtheme_set_font(ui->font);
    Rborder_set_background(ui->prompt_box, GUI_COLOR(ui, raised), 1);
    Rborder_set_outline(ui->prompt_box, 2, GUI_COLOR(ui, accent), 1);
}

int gui_theme_count(void)
{
    return (int)(sizeof(themes) / sizeof(themes[0]));
}

const GuiTheme *gui_theme_get(int index)
{
    return &themes[index >= 0 && index < gui_theme_count() ? index : 1];
}

const GuiTheme *gui_theme_current(const Gui *ui)
{
    return gui_theme_get(ui->theme);
}

const char *gui_theme_legend(void)
{
    return legend_text;
}
