#ifndef CCURATE_GUI_RENDER_H
#define CCURATE_GUI_RENDER_H

/* Canvas rendering and shared text drawing helpers. */
#include "gui_types.h"
#include <Raven.h>

void gui_text_clip(Gui *ui, RPainter *p, float x, float y, float w, float h, const char *s, float r,
                   float g, float b, float size);
void gui_multiline(Gui *ui, RPainter *p, const char *s, float x, float y, float w, float h,
                   int scroll);
void gui_render(Gui *ui, RPainter *p, float w, float h);

#endif /* CCURATE_GUI_RENDER_H */
