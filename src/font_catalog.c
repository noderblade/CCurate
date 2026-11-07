/* Fontconfig discovery and font file reads are isolated from the renderer. */
#include "font_catalog.h"
#include <errno.h>
#include <fcntl.h>
#include <fontconfig/fontconfig.h>
#ifdef _WIN32
#include "platform/windows/fonts_windows.h"
#else
#include <pwd.h>
#endif
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct FontService {
    Catalog *catalog;
    pthread_t worker;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    atomic_int stop, loading;
    atomic_ullong serial;
    int reload, index;
    char *file;
    MonoFonts *ready_list;
    FontBytes *ready_bytes;
};
static int font_order(const void *a, const void *b)
{
    const MonoFont *x = a, *y = b;
    int cmp = strcasecmp(x->family, y->family);
    if (!cmp)
        cmp = strcasecmp(x->style, y->style);
    if (!cmp)
        cmp = strcmp(x->file, y->file);
    return cmp ? cmp : (x->index > y->index) - (x->index < y->index);
}
static void invoking_user_fonts(FcConfig *config)
{
#ifdef _WIN32
    cc_win_font_dirs(config);
#else
    /* sudo normally changes HOME. Include the invoking user's standard font
     * directories without changing process environment or user configuration. */
    if (geteuid() != 0)
        return;
    const char *value = getenv("SUDO_UID");
    if (!value || !*value)
        return;
    char *end;
    errno = 0;
    unsigned long uid = strtoul(value, &end, 10);
    if (errno || *end || !uid || uid != (uid_t)uid)
        return;
    struct passwd pw, *found = NULL;
    char buffer[65536];
    if (getpwuid_r((uid_t)uid, &pw, buffer, sizeof(buffer), &found) || !found)
        return;
    const char *tails[] = {".local/share/fonts", ".fonts"};
    for (int i = 0; i < 2; i++) {
        char *dir = join_path(pw.pw_dir, tails[i]);
        if (!access(dir, R_OK | X_OK))
            FcConfigAppFontAddDir(config, (const FcChar8 *)dir);
        free(dir);
    }
#endif
}
MonoFonts *mono_fonts_find(void)
{
    MonoFonts *out = xmalloc(sizeof(*out));
    out->default_index = -1;
    FcConfig *config = FcInitLoadConfigAndFonts();
    if (!config) {
        snprintf(out->error, sizeof(out->error), "Fontconfig could not discover system fonts.");
        return out;
    }
    invoking_user_fonts(config);
    FcPattern *pattern = FcPatternCreate();
    FcObjectSet *objects = FcObjectSetBuild(FC_FAMILY, FC_STYLE, FC_FILE, FC_INDEX, FC_SPACING,
                                            FC_SCALABLE, FC_CHARSET, FC_FONTFORMAT, NULL);
    FcFontSet *set = pattern && objects ? FcFontList(config, pattern, objects) : NULL;
    int capacity = 0;
    if (set)
        for (int i = 0; i < set->nfont; i++) {
            FcPattern *p = set->fonts[i];
            FcChar8 *family = NULL, *style = NULL, *file = NULL, *format = NULL;
            FcCharSet *charset = NULL;
            FcBool scalable = FcFalse;
            int spacing = 0, index = 0;
            if (FcPatternGetInteger(p, FC_SPACING, 0, &spacing) != FcResultMatch ||
                (spacing != FC_MONO && spacing != FC_DUAL && spacing != FC_CHARCELL))
                continue;
            if (FcPatternGetBool(p, FC_SCALABLE, 0, &scalable) != FcResultMatch || !scalable)
                continue;
            if (FcPatternGetString(p, FC_FONTFORMAT, 0, &format) != FcResultMatch ||
                (strcmp((char *)format, "TrueType") && strcmp((char *)format, "CFF")))
                continue;
            if (FcPatternGetString(p, FC_FAMILY, 0, &family) != FcResultMatch ||
                FcPatternGetString(p, FC_FILE, 0, &file) != FcResultMatch ||
                FcPatternGetCharSet(p, FC_CHARSET, 0, &charset) != FcResultMatch)
                continue;
            int ascii = 1;
            for (int ch = 32; ch < 127; ch++)
                if (!FcCharSetHasChar(charset, ch)) {
                    ascii = 0;
                    break;
                }
            if (!ascii)
                continue; /* A symbols-only face is not a usable UI font. */
            FcPatternGetInteger(p, FC_INDEX, 0, &index);
            if (index < 0 || (index & 0xffff0000))
                continue; /* Named variable instances need axis support. */
            FcPatternGetString(p, FC_STYLE, 0, &style);
            if (out->count == capacity) {
                capacity = capacity ? capacity * 2 : 32;
                MonoFont *next = realloc(out->fonts, (size_t)capacity * sizeof(*next));
                if (!next)
                    abort();
                out->fonts = next;
            }
            MonoFont *f = &out->fonts[out->count++];
            f->family = xstrdup((char *)family);
            f->style = xstrdup(style ? (char *)style : "Regular");
            f->file = xstrdup((char *)file);
            f->index = index;
            char *safe_family = display_path(f->family), *safe_style = display_path(f->style);
            f->label = fmt("%s / %s", safe_family, safe_style);
            free(safe_family);
            free(safe_style);
        }
    if (set)
        FcFontSetDestroy(set);
    if (objects)
        FcObjectSetDestroy(objects);
    if (pattern)
        FcPatternDestroy(pattern);
    if (out->count > 1)
        qsort(out->fonts, out->count, sizeof(*out->fonts), font_order);
    /* Ask the desktop configuration for its preferred monospace, rather than
     * choosing a hardcoded family. Only accept matches in the filtered list. */
    pattern = FcNameParse((const FcChar8 *)"monospace:style=Regular:scalable=true");
    if (pattern) {
        FcConfigSubstitute(config, pattern, FcMatchPattern);
        FcDefaultSubstitute(pattern);
        FcResult result;
        set = FcFontSort(config, pattern, FcFalse, NULL, &result);
        if (set) {
            for (int i = 0; i < set->nfont && out->default_index < 0; i++) {
                FcChar8 *file = NULL;
                int index = 0;
                FcPatternGetInteger(set->fonts[i], FC_INDEX, 0, &index);
                if (FcPatternGetString(set->fonts[i], FC_FILE, 0, &file) != FcResultMatch)
                    continue;
                for (int j = 0; j < out->count; j++)
                    if (out->fonts[j].index == index && !strcmp(out->fonts[j].file, (char *)file)) {
                        out->default_index = j;
                        break;
                    }
            }
            FcFontSetDestroy(set);
        }
        FcPatternDestroy(pattern);
    }
    if (out->default_index < 0 && out->count)
        out->default_index = 0;
    if (!out->count)
        snprintf(out->error, sizeof(out->error),
                 "No compatible system monospace fonts found. Bundled Roboto Mono is available.");
    FcConfigDestroy(config);
    return out;
}
void mono_fonts_free(MonoFonts *list)
{
    if (!list)
        return;
    for (int i = 0; i < list->count; i++) {
        free(list->fonts[i].family);
        free(list->fonts[i].style);
        free(list->fonts[i].file);
        free(list->fonts[i].label);
    }
    free(list->fonts);
    free(list);
}
void font_bytes_free(FontBytes *data)
{
    if (data) {
        free(data->bytes);
        free(data);
    }
}
static uint32_t be32(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static FontBytes *read_face(FontService *s, const char *file, int index, uint64_t serial)
{
    FontBytes *out = xmalloc(sizeof(*out));
    out->serial = serial;
    int fd = open(file, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    struct stat st;
    if (fd < 0) {
        snprintf(out->error, sizeof(out->error), "Cannot read font: %s", strerror(errno));
        return out;
    }
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size < 12 ||
        st.st_size > 64 * 1024 * 1024) {
        snprintf(out->error, sizeof(out->error),
                 "Font must be a regular file between 12 bytes and 64 MiB.");
        close(fd);
        return out;
    }
    out->size = (size_t)st.st_size;
    out->bytes = xmalloc(out->size);
    size_t at = 0;
    while (at < out->size && !atomic_load(&s->stop) && serial == atomic_load(&s->serial)) {
        size_t size = out->size - at;
        if (size > 1024 * 1024)
            size = 1024 * 1024;
        ssize_t n = read(fd, out->bytes + at, size);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            snprintf(out->error, sizeof(out->error), "Font read failed: %s",
                     n < 0 ? strerror(errno) : "unexpected end of file");
            break;
        }
        at += (size_t)n;
    }
    close(fd);
    if (at != out->size && !*out->error)
        snprintf(out->error, sizeof(out->error), "Font loading cancelled.");
    if (!*out->error && !memcmp(out->bytes, "ttcf", 4)) {
        uint32_t count = be32(out->bytes + 8);
        if (index < 0 || (uint32_t)index >= count || 12ULL + 4ULL * count > out->size ||
            be32(out->bytes + 12 + 4 * (size_t)index) > out->size - 12) {
            snprintf(out->error, sizeof(out->error), "Invalid font collection face.");
        } else {
            /* Raven loads the first TTC face. Point that entry to the requested
             * face in this private memory copy; table offsets remain absolute. */
            memmove(out->bytes + 12, out->bytes + 12 + 4 * (size_t)index, 4);
        }
    } else if (!*out->error && index != 0)
        snprintf(out->error, sizeof(out->error),
                 "This file does not contain the selected font face.");
    if (*out->error) {
        free(out->bytes);
        out->bytes = NULL;
        out->size = 0;
    }
    return out;
}
static void *font_worker(void *arg)
{
    FontService *s = arg;
    for (;;) {
        pthread_mutex_lock(&s->mutex);
        while (!s->reload && !s->file && !atomic_load(&s->stop))
            pthread_cond_wait(&s->cond, &s->mutex);
        if (atomic_load(&s->stop)) {
            pthread_mutex_unlock(&s->mutex);
            break;
        }
        int reload = s->reload;
        s->reload = 0;
        char *file = reload ? NULL : s->file;
        int index = s->index;
        uint64_t serial = atomic_load(&s->serial);
        if (!reload)
            s->file = NULL;
        pthread_mutex_unlock(&s->mutex);
        if (reload) {
            MonoFonts *list = mono_fonts_find();
            activity_log(s->catalog, *list->error ? LOG_WARNING : LOG_DEBUG, "fonts.discovered",
                         NULL, NULL, "%d compatible monospace faces. %s", list->count, list->error);
            pthread_mutex_lock(&s->mutex);
            mono_fonts_free(s->ready_list);
            s->ready_list = list;
            atomic_store(&s->loading, s->reload != 0);
            pthread_mutex_unlock(&s->mutex);
        } else {
            FontBytes *data = read_face(s, file, index, serial);
            if (*data->error && serial == atomic_load(&s->serial))
                activity_log(s->catalog, LOG_WARNING, "font.read", file, NULL, "%s", data->error);
            pthread_mutex_lock(&s->mutex);
            if (serial == atomic_load(&s->serial)) {
                font_bytes_free(s->ready_bytes);
                s->ready_bytes = data;
            } else
                font_bytes_free(data);
            pthread_mutex_unlock(&s->mutex);
            free(file);
        }
    }
    return NULL;
}
FontService *font_service_start(Catalog *catalog)
{
    FontService *s = xmalloc(sizeof(*s));
    s->catalog = catalog;
    s->reload = 1;
    atomic_store(&s->loading, 1);
    pthread_mutex_init(&s->mutex, NULL);
    pthread_cond_init(&s->cond, NULL);
    if (pthread_create(&s->worker, NULL, font_worker, s)) {
        pthread_mutex_destroy(&s->mutex);
        pthread_cond_destroy(&s->cond);
        free(s);
        return NULL;
    }
    return s;
}
void font_service_reload(FontService *s)
{
    if (!s)
        return;
    pthread_mutex_lock(&s->mutex);
    s->reload = 1;
    atomic_store(&s->loading, 1);
    atomic_fetch_add(&s->serial, 1);
    free(s->file);
    s->file = NULL;
    pthread_cond_signal(&s->cond);
    pthread_mutex_unlock(&s->mutex);
}
int font_service_loading(FontService *s)
{
    return s && atomic_load(&s->loading);
}
MonoFonts *font_service_take_list(FontService *s)
{
    if (!s)
        return NULL;
    pthread_mutex_lock(&s->mutex);
    MonoFonts *out = s->ready_list;
    s->ready_list = NULL;
    pthread_mutex_unlock(&s->mutex);
    return out;
}
uint64_t font_service_request(FontService *s, const char *file, int index)
{
    if (!s)
        return 0;
    pthread_mutex_lock(&s->mutex);
    free(s->file);
    s->file = xstrdup(file);
    s->index = index;
    uint64_t serial = atomic_fetch_add(&s->serial, 1) + 1;
    pthread_cond_signal(&s->cond);
    pthread_mutex_unlock(&s->mutex);
    return serial;
}
FontBytes *font_service_take_bytes(FontService *s)
{
    if (!s)
        return NULL;
    pthread_mutex_lock(&s->mutex);
    FontBytes *out = s->ready_bytes;
    s->ready_bytes = NULL;
    pthread_mutex_unlock(&s->mutex);
    return out;
}
void font_service_stop(FontService *s)
{
    if (!s)
        return;
    atomic_store(&s->stop, 1);
    pthread_mutex_lock(&s->mutex);
    pthread_cond_signal(&s->cond);
    pthread_mutex_unlock(&s->mutex);
    pthread_join(s->worker, NULL);
    mono_fonts_free(s->ready_list);
    font_bytes_free(s->ready_bytes);
    free(s->file);
    pthread_mutex_destroy(&s->mutex);
    pthread_cond_destroy(&s->cond);
    free(s);
}
