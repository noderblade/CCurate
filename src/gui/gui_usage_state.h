#ifndef CCURATE_GUI_USAGE_STATE_H
#define CCURATE_GUI_USAGE_STATE_H

#include "gui_layout.h"
#include "gui_usage.h"
#include "usage.h"

enum { USAGE_BUTTONS = 15, USAGE_LOCATIONS = 32, USAGE_CRUMBS = 32 };
typedef struct {
    char *path, *selection;
    int page, offset, horizontal;
} GuiUsageLocation;
typedef struct {
    GuiRect tree, chart, details, body, tree_split, detail_split, breadcrumb;
    GuiRect buttons[USAGE_BUTTONS], columns[USAGE_COLUMNS];
    GuiRect crumbs[USAGE_CRUMBS];
    size_t crumb_end[USAGE_CRUMBS];
    int crumb_count;
    int visible;
    float table_width;
} GuiUsageGeometry;
typedef struct {
    GuiRect rect;
    float a0, a1, r0, r1, cx, cy;
    int active;
} GuiUsageShape;
typedef struct GuiUsage {
    UsageService *service;
    UsageRequest request;
    UsageResult *result;
    UsageStatus status;
    uint64_t serial;
    int loading, selected, offset, horizontal, details_scroll, details_horizontal, details_lines;
    float details_width, split, details_split, widths[USAGE_COLUMNS];
    unsigned columns;
    int treemap, drag, column_drag, column_hover, popup, popup_row, help, focus, hover;
    float drag_x, drag_width;
    GuiUsageLocation locations[USAGE_LOCATIONS];
    unsigned location_next;
    GuiUsageShape shapes[USAGE_CHART_LIMIT];
    GuiRect center;
    char *error;
} GuiUsage;

GuiUsageGeometry gui_usage_geometry(Gui *ui);
void gui_usage_request(Gui *ui);
void gui_usage_navigate(Gui *ui, const char *path);
void gui_usage_select(Gui *ui, int row);
void gui_usage_expand(Gui *ui, int row);
void gui_usage_fit(Gui *ui, int column);
void gui_usage_keep_visible(Gui *ui);
void gui_usage_chart_paint(Gui *ui, RPainter *p, GuiRect area);
int gui_usage_chart_hit(Gui *ui, float x, float y);
unsigned gui_usage_node_color(Gui *ui, int index);
const char *gui_usage_column_name(int column);
const char *gui_usage_button_command(int button);
void gui_usage_cell(Gui *ui, const UsageRow *row, int column, char *out, size_t size);

#endif
