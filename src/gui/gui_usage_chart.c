#include "gui_render.h"
#include "gui_state.h"
#include "gui_theme.h"
#include "gui_usage_state.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define USAGE_PI 3.14159265358979323846
static unsigned shade(unsigned color, double amount)
{
    unsigned result = 0;
    for (int shift = 0; shift <= 16; shift += 8) {
        double channel = (color >> shift) & 255;
        channel += amount * (amount > 0 ? 255 - channel : channel);
        result |= (unsigned)lround(channel) << shift;
    }
    return result;
}
unsigned gui_usage_node_color(Gui *ui, int index)
{
    GuiUsage *u = ui->usage;
    int original = index, sibling = 0;
    for (int i = 0; i < index; i++)
        if (u->result->nodes[i].parent == u->result->nodes[index].parent)
            sibling++;
    while (u->result->nodes[index].parent >= 0)
        index = u->result->nodes[index].parent;
    const char *path = u->result->nodes[index].row.path;
    unsigned hash = 2166136261u;
    for (const unsigned char *s = (const unsigned char *)path; *s; s++)
        hash = (hash ^ *s) * 16777619u;
    const GuiTheme *t = gui_theme_current(ui);
    unsigned colors[] = {t->accent, t->directory, t->duplicate, t->success, t->warning, t->muted};
    /* Keep a branch's hue, alternating lightness across rings and siblings.
     * Adjacent levels remain distinct without cutting gaps into the data. */
    static const double levels[] = {0, -.20, .16, -.28, .24};
    int depth = u->result->nodes[original].row.depth;
    double lightness = levels[depth >= 0 && depth < 5 ? depth : 0];
    return shade(colors[hash % 6], lightness + (sibling % 2 ? .07 : -.07));
}
static void triangle(RPainter *p, float x0, float y0, float x1, float y1, float x2, float y2,
                     unsigned color)
{
    /* Raven's triangle SDF has zero orientation for coincident/collinear
     * vertices, painting its entire bounding rectangle at half opacity.
     * Check the actual float vertices sent to the renderer, not just angles. */
    float area = (x1 - x0) * (y2 - y0) - (y1 - y0) * (x2 - x0);
    if (fabsf(area) > .0001f)
        Rdraw_triangle(p, x0, y0, x1, y1, x2, y2, GUI_HEX_COLOR(color), 1);
}
static void arc(Gui *ui, RPainter *p, const GuiUsageShape *s, unsigned color, int hot)
{
    /* Raven antialiases each triangle separately. Opaque seam underlays avoid
     * dark internal tessellation lines without changing the vendored renderer. */
    unsigned mixed = hot ? shade(color, .22) : color;
    double span = s->a1 - s->a0;
    /* A byte-sized entry in a TiB-scale chart is smaller than a screen pixel.
     * Retain its exact angular allocation and hit target, but do not inflate
     * it into a minimum-width stroke or submit a degenerate primitive. */
    if (span * s->r1 * ui->scale < .1)
        return;
    int steps = (int)ceil(span * fmaxf(2, s->r1) / 5);
    if (steps < 1)
        steps = 1;
    if (steps > 600)
        steps = 600;
    for (int i = 0; i < steps; i++) {
        double a = s->a0 + span * i / steps, b = s->a0 + span * (i + 1) / steps;
        float x0 = s->cx + cos(a) * s->r0, y0 = s->cy + sin(a) * s->r0, x1 = s->cx + cos(a) * s->r1,
              y1 = s->cy + sin(a) * s->r1;
        float x2 = s->cx + cos(b) * s->r1, y2 = s->cy + sin(b) * s->r1, x3 = s->cx + cos(b) * s->r0,
              y3 = s->cy + sin(b) * s->r0;
        /* Cover the full one-physical-pixel antialiasing band before the
         * triangles are composited, including at UI scales below 100%. */
        float seam = 2.0f / ui->scale;
        Rdraw_line(p, x0, y0, x2, y2, seam, GUI_HEX_COLOR(mixed), 1);
        if (i)
            Rdraw_line(p, x0, y0, x1, y1, seam, GUI_HEX_COLOR(mixed), 1);
        triangle(p, x0, y0, x1, y1, x2, y2, mixed);
        triangle(p, x0, y0, x2, y2, x3, y3, mixed);
        Rdraw_line(p, x1, y1, x2, y2, 1.4f, GUI_HEX_COLOR(mixed), 1);
        Rdraw_line(p, x0, y0, x3, y3, 1.4f, GUI_HEX_COLOR(mixed), 1);
    }
}
static void treemap(Gui *ui, RPainter *p, GuiRect area, int parent)
{
    GuiUsage *u = ui->usage;
    int64_t bytes =
        parent < 0 ? u->result->totals.bytes : u->result->nodes[parent].row.totals.bytes;
    if (bytes <= 0 || area.w < 4 || area.h < 4)
        return;
    float offset = 0;
    for (int i = 0; i < u->result->node_count; i++) {
        UsageNode *node = u->result->nodes + i;
        if (node->parent != parent)
            continue;
        float fraction = (double)node->row.totals.bytes / bytes;
        int vertical = area.w >= area.h;
        GuiRect r = {area.x + (vertical ? offset * area.w : 0),
                     area.y + (vertical ? 0 : offset * area.h),
                     vertical ? fraction * area.w : area.w, vertical ? area.h : fraction * area.h};
        offset += fraction;
        r.x += 1;
        r.y += 1;
        r.w = fmaxf(0, r.w - 2);
        r.h = fmaxf(0, r.h - 2);
        u->shapes[i] = (GuiUsageShape){.rect = r, .active = 1};
        Rdraw_rect(p, r.x, r.y, r.w, r.h, 0, GUI_HEX_COLOR(gui_usage_node_color(ui, i)),
                   u->hover == i ? .88f : .45f);
        if (r.w > 65 && r.h > 28) {
            char *safe = display_path(node->row.name);
            gui_text_clip(ui, p, r.x + 6, r.y + 5, r.w - 12, 20, safe, GUI_COLOR(ui, text), 12);
            free(safe);
            if (r.w > 110 && r.h > 58) {
                char label[64];
                snprintf(label, sizeof(label), "%.1f%%",
                         100.0 * node->row.totals.bytes / u->result->totals.bytes);
                gui_text_clip(ui, p, r.x + 6, r.y + 23, r.w - 12, 20, label, GUI_COLOR(ui, text),
                              11);
            }
        }
        if (r.h > 85 && r.w > 70)
            treemap(ui, p, (GuiRect){r.x + 4, r.y + 44, r.w - 8, r.h - 48}, i);
    }
}
void gui_usage_chart_paint(Gui *ui, RPainter *p, GuiRect area)
{
    GuiUsage *u = ui->usage;
    memset(u->shapes, 0, sizeof(u->shapes));
    u->center = (GuiRect){0};
    if (!u->result || area.w < 10 || area.h < 10)
        return;
    if (u->result->totals.bytes <= 0) {
        gui_text_clip(ui, p, area.x + 10, area.y + 15, area.w - 20, 60,
                      "No file content in this directory.", GUI_COLOR(ui, muted), 13);
        return;
    }
    Rdraw_push_clip_rect(p, area.x, area.y, area.w, area.h);
    if (u->treemap) {
        treemap(ui, p, area, -1);
        Rdraw_pop_clip_rect(p);
        return;
    }
    float cx = area.x + area.w / 2, cy = area.y + area.h / 2,
          outer = fmaxf(1, fminf(area.w, area.h) / 2 - 5);
    float inner = fminf(62, outer * .38f), ring = (outer - inner) / u->request.depth;
    int64_t consumed[USAGE_CHART_LIMIT + 1] = {0};
    for (int i = 0; i < u->result->node_count; i++) {
        UsageNode *node = u->result->nodes + i;
        GuiUsageShape *s = u->shapes + i;
        double base = node->parent < 0 ? -USAGE_PI / 2 : u->shapes[node->parent].a0;
        double span = node->parent < 0 ? 2 * USAGE_PI
                                       : u->shapes[node->parent].a1 - u->shapes[node->parent].a0;
        int64_t bytes = node->parent < 0 ? u->result->totals.bytes
                                         : u->result->nodes[node->parent].row.totals.bytes;
        int64_t before = consumed[node->parent + 1];
        consumed[node->parent + 1] += node->row.totals.bytes;
        s->a0 = base + (bytes ? span * ((double)before / bytes) : 0);
        s->a1 = base + (bytes ? span * ((double)consumed[node->parent + 1] / bytes) : 0);
        s->r0 = inner + node->row.depth * ring;
        s->r1 = inner + (node->row.depth + 1) * ring;
        s->cx = cx;
        s->cy = cy;
        s->active = s->r1 > s->r0 && s->a1 > s->a0;
        if (s->active)
            arc(ui, p, s, gui_usage_node_color(ui, i), u->hover == i);
    }
    u->center = (GuiRect){cx - inner, cy - inner, inner * 2, inner * 2};
    if (inner > 24) {
        char bytes[40];
        human_bytes(u->result->totals.bytes, bytes);
        float width = 0;
        Rfont_measure_text(ui->font, 13, bytes, &width, NULL);
        gui_text_clip(ui, p, cx - width / 2, cy - 11, inner * 2, 22, bytes, GUI_COLOR(ui, text),
                      13);
        if (*u->request.path)
            gui_text_clip(ui, p, cx - 22, cy + 11, inner * 2, 20, "Up", GUI_COLOR(ui, muted), 11);
    }
    Rdraw_pop_clip_rect(p);
}
int gui_usage_chart_hit(Gui *ui, float x, float y)
{
    GuiUsage *u = ui->usage;
    if (!u->result)
        return -1;
    for (int i = u->result->node_count - 1; i >= 0; i--) {
        GuiUsageShape *s = u->shapes + i;
        if (!s->active)
            continue;
        if (u->treemap) {
            if (gui_contains(s->rect, x, y))
                return i;
            continue;
        }
        float dx = x - s->cx, dy = y - s->cy, r = hypotf(dx, dy);
        double a = atan2(dy, dx);
        if (a < -USAGE_PI / 2)
            a += 2 * USAGE_PI;
        if (r >= s->r0 && r <= s->r1 && a >= s->a0 && a <= s->a1)
            return i;
    }
    return -1;
}
