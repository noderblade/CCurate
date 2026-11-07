#include "curator.h"
#include "platform/windows/preview_windows.h"
#include <gio/gio.h>

PopplerDocument *cc_win_pdf_document(int fd, GError **error)
{
    struct stat st;
    if (fstat(fd, &st) || st.st_size < 0 || st.st_size > 256LL * 1024 * 1024) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "Cannot read PDF or PDF exceeds the 256 MiB Windows preview limit");
        return NULL;
    }
    size_t size = (size_t)st.st_size, offset = 0;
    unsigned char *data = xmalloc(size);
    while (offset < size) {
        ssize_t n = pread(fd, data + offset, size - offset, (int64_t)offset);
        if (n <= 0) {
            free(data);
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "PDF changed or became unreadable during preview");
            return NULL;
        }
        offset += (size_t)n;
    }
    GBytes *bytes = g_bytes_new_take(data, size);
    GInputStream *stream = g_memory_input_stream_new_from_bytes(bytes);
    PopplerDocument *document =
        poppler_document_new_from_stream(stream, (goffset)size, NULL, NULL, error);
    g_object_unref(stream);
    g_bytes_unref(bytes);
    return document;
}
