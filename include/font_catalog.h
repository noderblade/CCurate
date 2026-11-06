#ifndef CCURATE_FONT_CATALOG_H
#define CCURATE_FONT_CATALOG_H
#include "curator.h"

typedef struct {
    char *family, *style, *file, *label;
    int index;
} MonoFont;
typedef struct {
    MonoFont *fonts;
    int count, default_index;
    char error[256];
} MonoFonts;
typedef struct {
    uint64_t serial;
    unsigned char *bytes;
    size_t size;
    char error[256];
} FontBytes;
typedef struct FontService FontService;

MonoFonts *mono_fonts_find(void);
void mono_fonts_free(MonoFonts *fonts);
FontService *font_service_start(Catalog *catalog);
void font_service_reload(FontService *s);
int font_service_loading(FontService *s);
MonoFonts *font_service_take_list(FontService *s);
uint64_t font_service_request(FontService *s, const char *file, int index);
FontBytes *font_service_take_bytes(FontService *s);
void font_bytes_free(FontBytes *bytes);
void font_service_stop(FontService *s);
#endif
