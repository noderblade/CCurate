#include "gui_tabs.h"
#include "gui_controller.h"
#include "gui_layout.h"
#include "gui_render.h"
#include "gui_state.h"
#include "gui_theme.h"
#include "gui_usage.h"

static GuiRect rect(Gui *ui, int tab)
{
    return (GuiRect){ui->width < 680 ? 12 + tab * 142 : 155 + tab * 142,
                     ui->width < 680 ? gui_pane_top(ui) - 38 : 8, 136, 28};
}
void gui_tabs_select(Gui *ui, int tab)
{
    ui->tab = !!tab;
    ui->menu = ui->dropdown = 0;
    ui->scroll_drag = -1;
    ui->drag = ui->column_drag = 0;
    if (ui->tab)
        gui_usage_activate(ui);
    gui_usage_save(ui);
    Rwidget_focus(ui->canvas);
    Rwidget_set_cursor(ui->canvas, R_CURSOR_ARROW);
}
void gui_tabs_paint(Gui *ui, RPainter *p)
{
    for (int i = 0; i < 2; i++) {
        GuiRect r = rect(ui, i);
        Rdraw_rect(p, r.x, r.y, r.w, r.h, 4,
                   GUI_HEX_COLOR(i == ui->tab ? gui_theme_current(ui)->selection
                                              : gui_theme_current(ui)->raised),
                   1);
        if (i == ui->tab)
            Rdraw_rect(p, r.x, r.y + r.h - 2, r.w, 2, 0, GUI_COLOR(ui, accent), 1);
        gui_text_clip(ui, p, r.x + 10, r.y + 6, r.w - 20, 22, i ? "Disk Usage" : "Curation",
                      GUI_COLOR(ui, text), 13);
    }
}
int gui_tabs_event(Gui *ui, const REvent *e)
{
    if (ui->font_dialog || ui->dialog || ui->dropdown || ui->menu)
        return 0;
    if (e->type == R_EVENT_KEY_DOWN && e->key == R_KEY_TAB && (e->modifiers & R_MOD_CTRL)) {
        gui_tabs_select(ui, !ui->tab);
        return 1;
    }
    if (e->type == R_EVENT_MOUSE_DOWN && e->button == R_MOUSE_LEFT) {
        for (int i = 0; i < 2; i++)
            if (gui_contains(rect(ui, i), e->x, e->y)) {
                gui_tabs_select(ui, i);
                return 1;
            }
    }
    return 0;
}
