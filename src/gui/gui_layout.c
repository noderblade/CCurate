#include "gui_layout.h"
#include "gui_controller.h"
#include "gui_state.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Panel geometry, hit testing and split proportions. */

static float gui_bounded_split(float ratio, float available, float minimum);

float gui_pane_top(Gui *ui)
{
    return ui->height < 420 ? 118 : 148;
}

static float gui_bounded_split(float ratio, float available, float minimum)
{
    minimum = fminf(minimum, available * .3f);
    return fmaxf(minimum, fminf(available - minimum, available * ratio));
}

GuiGeometry gui_geometry(Gui *ui)
{
    float top = gui_pane_top(ui), aw = fmaxf(1, ui->width - 36),
          total = fmaxf(1, ui->height - top - 54);
    float log_h = ui->log_visible
                      ? fminf(gui_bounded_split(ui->split_log, total, 96), fmaxf(30, total - 210))
                      : 0;
    float ah = fmaxf(1, total - (ui->log_visible ? log_h + 12 : 0));
    float tw = gui_bounded_split(ui->split_top, aw, 170),
          bw = gui_bounded_split(ui->split_bottom, aw, 170);
    float th = gui_bounded_split(ui->split_rows, ah, 105), by = top + th + 12;
    GuiGeometry g = {.pane = {{12, top, tw, th},
                              {24 + tw, top, aw - tw, th},
                              {12, by, bw, ah - th},
                              {24 + bw, by, aw - bw, ah - th},
                              {12, top + ah + 24, ui->width - 24, log_h}},
                     .split = {{12 + tw, top, 12, th},
                               {12 + bw, by, 12, ah - th},
                               {12, top + th, ui->width - 24, 12},
                               {12, top + ah + 12, ui->width - 24, ui->log_visible ? 12 : 0}}};
    if (!ui->log_visible)
        g.pane[4].h = 0;
    return g;
}

int gui_contains(GuiRect r, float x, float y)
{
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

int gui_pane_at(GuiGeometry g, float x, float y)
{
    for (int i = 0; i < 5; i++)
        if (gui_contains(g.pane[i], x, y))
            return i;
    return -1;
}

float gui_pane_height(Gui *ui)
{
    return gui_geometry(ui).pane[0].h;
}

void gui_reset_layout(Gui *ui)
{
    ui->split_top = ui->split_bottom = .5f;
    ui->split_rows = .53f;
    ui->split_log = .25f;
    gui_save_ui(ui);
}

GuiTable gui_table(Gui *ui, int side)
{
    GuiRect r = gui_geometry(ui).pane[side];
    GuiTable t = {.body = {r.x + 8, r.y + 70, fmaxf(1, r.w - 28), fmaxf(1, r.h - 110)}};
    float cell = 0;
    Rfont_measure_text(ui->font, ui->font_size, "M", &cell, NULL);
    t.widths[0] =
        fmaxf(fmaxf(240, t.body.w * .39f), ui->pane[side].name_chars * fmaxf(1, cell) + 36);
    t.widths[1] = fmaxf(155, t.body.w * .19f);
    t.widths[2] = fmaxf(106, t.body.w * .13f);
    t.widths[3] = fmaxf(166, t.body.w * .20f);
    t.widths[4] = 80;
    for (int i = 0; i < 5; i++)
        t.width += t.widths[i];
    return t;
}

int gui_table_visible(Gui *ui, int side)
{
    return (int)fmaxf(1, gui_table(ui, side).body.h / ui->row_height);
}
