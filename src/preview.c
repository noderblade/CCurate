#include "curator.h"
#include <archive.h>
#include <archive_entry.h>
#include <errno.h>
#include <fcntl.h>
#ifndef _WIN32
#include <grp.h>
#include <linux/filter.h>
#include <linux/netlink.h>
#include <linux/seccomp.h>
#endif
#include <magic.h>
#ifndef _WIN32
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#endif
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#endif
#include <unistd.h>
#ifdef CCURATE_MEDIA
#include "preview_jpeg.h"
#include <cairo.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <poppler.h>
#ifdef _WIN32
#include "platform/windows/preview_windows.h"
#endif
#endif

#ifndef _WIN32
extern char **environ;
#endif
void preview_free(Preview *p)
{
    free(p->rgba);
    free(p->text);
    memset(p, 0, sizeof(*p));
}
static void hex_preview(int fd, Buf *b)
{
    unsigned char buf[2048];
    ssize_t n = pread(fd, buf, sizeof(buf), 0);
    if (n < 0) {
        badd(b, "read: %s\n", strerror(errno));
        return;
    }
    for (ssize_t i = 0; i < n; i += 16) {
        badd(b, "%08llx  ", (unsigned long long)i);
        for (int j = 0; j < 16; j++)
            if (i + j < n)
                badd(b, "%02x ", buf[i + j]);
            else
                badd(b, "   ");
        badd(b, " ");
        for (int j = 0; j < 16 && i + j < n; j++)
            badd(b, "%c", buf[i + j] >= 32 && buf[i + j] < 127 ? buf[i + j] : '.');
        badd(b, "\n");
    }
    badd(b, "\nFirst %zd bytes; the original is unchanged.\n", n);
}
static int archive_preview(int fd, Buf *b)
{
    struct archive *a = archive_read_new();
    archive_read_support_filter_all(a);
    archive_read_support_format_all(a);
    int af = dup(fd);
    lseek(af, 0, SEEK_SET);
    if (archive_read_open_fd(a, af, 64 * 1024) != ARCHIVE_OK) {
        archive_read_free(a);
        close(af);
        return 0;
    }
    struct archive_entry *e;
    int n = 0, rc;
    Buf tmp = {0};
    badd(&tmp, "ARCHIVE: %s\nContents (without extraction):\n\n",
         archive_format_name(a) ? archive_format_name(a) : "");
    while ((rc = archive_read_next_header(a, &e)) == ARCHIVE_OK) {
        if (n == 3000) {
            badd(&tmp, "\n... preview limited to 3000 entries; nothing was extracted.\n");
            break;
        }
        const char *name = archive_entry_pathname(e);
        char *d = display_path(name ? name : "[name unavailable]"), size[40];
        human_bytes(archive_entry_size(e), size);
        badd(&tmp, "%10s  %s%s\n", size, d, archive_entry_filetype(e) == AE_IFDIR ? "/" : "");
        free(d);
        n++;
        if (archive_read_data_skip(a) < ARCHIVE_WARN)
            break;
    }
    if (rc < ARCHIVE_WARN)
        badd(&tmp, "\nError / encryption: %s\n",
             archive_error_string(a) ? archive_error_string(a) : "unknown");
    archive_read_free(a);
    close(af);
    if (n || rc == ARCHIVE_EOF) {
        badd(b, "%s", tmp.s);
        bfree(&tmp);
        return 1;
    }
    bfree(&tmp);
    return 0;
}
#ifdef CCURATE_MEDIA
static void image_size(GdkPixbufLoader *l, int w, int h, void *unused)
{
    int *original = unused;
    original[0] = w;
    original[1] = h;
    double scale = 1.0;
    if (w > 1000)
        scale = 1000.0 / w;
    if (h * scale > 800)
        scale = 800.0 / h;
    gdk_pixbuf_loader_set_size(l, (int)(w * scale) < 1 ? 1 : (int)(w * scale),
                               (int)(h * scale) < 1 ? 1 : (int)(h * scale));
}
static int image_preview(int fd, Preview *p, Buf *b)
{
    /* Retained by the parent if the decoder exits before sending its packet. */
    fprintf(stderr, "Image preview backend: GdkPixbuf %u.%u.%u\n", gdk_pixbuf_major_version,
            gdk_pixbuf_minor_version, gdk_pixbuf_micro_version);
    GError *err = NULL;
    GdkPixbufLoader *l = gdk_pixbuf_loader_new();
    int original[2] = {0};
    g_signal_connect(l, "size-prepared", G_CALLBACK(image_size), original);
    unsigned char buf[65536];
    off_t offset = 0;
    int ok = 1;
    while (offset < 256 * 1024 * 1024) {
        ssize_t n = pread(fd, buf, sizeof(buf), offset);
        if (n < 0) {
            badd(b, "Image read failed: %s\n", strerror(errno));
            ok = 0;
            break;
        }
        if (n == 0)
            break;
        if (!gdk_pixbuf_loader_write(l, buf, n, &err)) {
            ok = 0;
            break;
        }
        offset += n;
    }
    if (!gdk_pixbuf_loader_close(l, err ? NULL : &err))
        ok = 0;
    GdkPixbuf *pix = ok ? gdk_pixbuf_loader_get_pixbuf(l) : NULL;
    if (pix) {
        GdkPixbuf *rot = gdk_pixbuf_apply_embedded_orientation(pix);
        pix = rot ? rot : pix;
        /* EXIF rotation can swap the axes after the loader has scaled them.
         * Fit the ORIENTED image again before crossing the IPC size boundary. */
        int oriented_w = gdk_pixbuf_get_width(pix), oriented_h = gdk_pixbuf_get_height(pix);
        GdkPixbuf *fit = NULL;
        if (oriented_w > 1000 || oriented_h > 800) {
            double scale = 1000.0 / oriented_w;
            if (oriented_h * scale > 800)
                scale = 800.0 / oriented_h;
            int w = (int)(oriented_w * scale), h = (int)(oriented_h * scale);
            fit = gdk_pixbuf_scale_simple(pix, w > 0 ? w : 1, h > 0 ? h : 1, GDK_INTERP_BILINEAR);
            if (!fit) {
                badd(b, "Image scaling failed: insufficient decoder memory.\n");
                p->error = 1;
                if (rot)
                    g_object_unref(rot);
                g_object_unref(l);
                if (err)
                    g_error_free(err);
                return 0;
            }
            pix = fit;
        }
        p->width = gdk_pixbuf_get_width(pix);
        p->height = gdk_pixbuf_get_height(pix);
        p->rgba = xmalloc((size_t)p->width * p->height * 4);
        int stride = gdk_pixbuf_get_rowstride(pix), channels = gdk_pixbuf_get_n_channels(pix);
        const guchar *v = gdk_pixbuf_read_pixels(pix);
        for (int y = 0; y < p->height; y++)
            for (int x = 0; x < p->width; x++) {
                const guchar *s = v + y * stride + x * channels;
                uint8_t *d = p->rgba + 4 * ((size_t)y * p->width + x);
                memcpy(d, s, 3);
                d[3] = channels == 4 ? s[3] : 255;
            }
        badd(b, "Original image: %d x %d px\nImage: oriented preview %d x %d px\n", original[0],
             original[1], p->width, p->height);
        GdkPixbufFormat *f = gdk_pixbuf_loader_get_format(l);
        if (f) {
            char *name = gdk_pixbuf_format_get_name(f);
            badd(b, "Decoder: %s\n", name);
            g_free(name);
        }
        const char *keys[] = {"x-dpi", "y-dpi", "orientation", "Title", "Author", NULL};
        for (int i = 0; keys[i]; i++) {
            const char *option = gdk_pixbuf_get_option(pix, keys[i]);
            if (option)
                badd(b, "%s: %s\n", keys[i], option);
        }
        if (fit)
            g_object_unref(fit);
        if (rot)
            g_object_unref(rot);
    }
    if (!p->rgba) {
        p->error = 1;
        badd(b, "Image decoder: %s\n",
             err ? err->message : "no pixel data (unsupported format or image input limit)");
    }
    if (err)
        g_error_free(err);
    g_object_unref(l);
    return p->rgba != NULL;
}
static int pdf_preview(int fd, int page, Preview *p, Buf *b)
{
    GError *err = NULL;
#ifdef _WIN32
    PopplerDocument *doc = cc_win_pdf_document(fd, &err);
#else
    int copy = dup(fd);
    lseek(copy, 0, SEEK_SET);
    PopplerDocument *doc = poppler_document_new_from_fd(copy, NULL, &err);
#endif
    if (!doc) {
        p->error = 1;
        badd(b, "PDF: %s\n", err ? err->message : "cannot decode");
        if (err)
            g_error_free(err);
        return 0;
    }
    p->pages = poppler_document_get_n_pages(doc);
    if (page < 0)
        page = 0;
    if (page >= p->pages)
        page = p->pages - 1;
    PopplerPage *pg = poppler_document_get_page(doc, page);
    if (!pg) {
        g_object_unref(doc);
        return 0;
    }
    double w, h;
    poppler_page_get_size(pg, &w, &h);
    double scale = 1000.0 / w;
    if (h * scale > 800)
        scale = 800.0 / h;
    p->width = (int)(w * scale);
    p->height = (int)(h * scale);
    if (p->width < 1 || p->height < 1) {
        g_object_unref(pg);
        g_object_unref(doc);
        return 0;
    }
    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, p->width, p->height);
    cairo_t *cr = cairo_create(surface);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_paint(cr);
    cairo_scale(cr, scale, scale);
    poppler_page_render(pg, cr);
    cairo_surface_flush(surface);
    p->rgba = xmalloc((size_t)p->width * p->height * 4);
    const unsigned char *data = cairo_image_surface_get_data(surface);
    int stride = cairo_image_surface_get_stride(surface);
    for (int y = 0; y < p->height; y++)
        for (int x = 0; x < p->width; x++) {
            const unsigned char *s = data + y * stride + 4 * x;
            unsigned char *d = p->rgba + 4 * ((size_t)y * p->width + x);
            d[0] = s[2];
            d[1] = s[1];
            d[2] = s[0];
            d[3] = 255;
        }
    char *title = poppler_document_get_title(doc), *author = poppler_document_get_author(doc);
    badd(b,
         "PDF  |  page %d / %d\nPage size: %.0f x %.0f pt\nTitle: %s\nAuthor: %s\n[ / ] "
         "changes page\n",
         page + 1, p->pages, w, h, title ? title : "", author ? author : "");
    g_free(title);
    g_free(author);
    cairo_destroy(cr);
    cairo_surface_destroy(surface);
    g_object_unref(pg);
    g_object_unref(doc);
    return 1;
}
typedef struct {
    int fd;
    off_t pos;
} Input;
static int avread(void *v, uint8_t *buf, int len)
{
    Input *in = v;
    ssize_t n = pread(in->fd, buf, len, in->pos);
    if (n > 0) {
        in->pos += n;
        return (int)n;
    }
    return n == 0 ? AVERROR_EOF : AVERROR(errno);
}
static int64_t avseek(void *v, int64_t offset, int whence)
{
    Input *in = v;
    struct stat st;
    if (fstat(in->fd, &st))
        return AVERROR(errno);
    if (whence & AVSEEK_SIZE)
        return st.st_size;
    whence &= ~AVSEEK_FORCE;
    int64_t pos = whence == SEEK_SET   ? offset
                  : whence == SEEK_CUR ? in->pos + offset
                  : whence == SEEK_END ? st.st_size + offset
                                       : -1;
    if (pos < 0)
        return AVERROR(EINVAL);
    in->pos = pos;
    return pos;
}
static int video_preview(int fd, int page, Preview *p, Buf *b)
{
    av_log_set_level(AV_LOG_ERROR);
    Input input = {fd, 0};
    AVIOContext *io = avio_alloc_context(av_malloc(65536), 65536, 0, &input, avread, NULL, avseek);
    if (!io)
        return 0;
    AVFormatContext *f = avformat_alloc_context();
    f->pb = io;
    f->flags |= AVFMT_FLAG_CUSTOM_IO;
    AVDictionary *opts = NULL;
    av_dict_set(&opts, "protocol_whitelist", "", 0);
    int rc = avformat_open_input(&f, NULL, NULL, &opts);
    av_dict_free(&opts);
    int got = 0;
    if (rc < 0)
        goto end;
    if (avformat_find_stream_info(f, NULL) < 0)
        goto end;
    badd(b, "Media: %s\nDuration: %.2f s\n", f->iformat->long_name,
         (double)f->duration / AV_TIME_BASE);
    for (unsigned i = 0; i < f->nb_streams; i++) {
        AVCodecParameters *q = f->streams[i]->codecpar;
        badd(b, "Stream %u: %s  %d x %d  %d Hz\n", i, avcodec_get_name(q->codec_id), q->width,
             q->height, q->sample_rate);
    }
    AVDictionaryEntry *tag = NULL;
    while ((tag = av_dict_get(f->metadata, "", tag, AV_DICT_IGNORE_SUFFIX)))
        badd(b, "%s: %.300s\n", tag->key, tag->value);
    const AVCodec *codec = NULL;
    int stream = av_find_best_stream(f, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (stream < 0) {
        got = 1;
        goto end;
    }
    AVCodecContext *dec = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(dec, f->streams[stream]->codecpar);
    dec->thread_count = 1;
    if (avcodec_open2(dec, codec, NULL) < 0) {
        avcodec_free_context(&dec);
        goto end;
    }
    double seconds = page > 0 ? page * 10.0 : 0;
    if (seconds > 0) {
        int64_t t = (int64_t)(seconds / av_q2d(f->streams[stream]->time_base));
        av_seek_frame(f, stream, t, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(dec);
    }
    AVPacket *packet = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    for (int i = 0; i < 2000 && !got && av_read_frame(f, packet) >= 0; i++) {
        if (packet->stream_index == stream && avcodec_send_packet(dec, packet) >= 0) {
            if (avcodec_receive_frame(dec, frame) == 0) {
                double scale = frame->width > 1000 ? 1000.0 / frame->width : 1;
                if (frame->height * scale > 800)
                    scale = 800.0 / frame->height;
                p->width = (int)(frame->width * scale);
                p->height = (int)(frame->height * scale);
                if (p->width > 0 && p->height > 0) {
                    struct SwsContext *sw =
                        sws_getContext(frame->width, frame->height, frame->format, p->width,
                                       p->height, AV_PIX_FMT_RGBA, SWS_BILINEAR, NULL, NULL, NULL);
                    if (sw) {
                        p->rgba = xmalloc((size_t)p->width * p->height * 4);
                        uint8_t *dst[4] = {p->rgba, NULL, NULL, NULL};
                        int lines[4] = {p->width * 4, 0, 0, 0};
                        sws_scale(sw, (const uint8_t *const *)frame->data, frame->linesize, 0,
                                  frame->height, dst, lines);
                        sws_freeContext(sw);
                        got = 1;
                        badd(b, "\nFrame near %.0f s; [ / ]: 10-second steps.\n", seconds);
                    }
                }
            }
        }
        av_packet_unref(packet);
    }
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&dec);
end:
    if (f)
        avformat_close_input(&f);
    av_freep(&io->buffer);
    avio_context_free(&io);
    return got;
}
#endif

int preview_read(int fd, int page, int hex_only, Preview *p)
{
    (void)page;
    memset(p, 0, sizeof(*p));
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode)) {
        p->text = xstrdup("Special object: byte preview is disabled.");
        return -1;
    }
    Buf b = {0};
    magic_t m = magic_open(MAGIC_MIME_TYPE | MAGIC_ERROR);
    char *mime = xstrdup("application/octet-stream");
    if (m && magic_load(m, NULL) == 0) {
        const char *s = magic_descriptor(m, fd);
        if (s) {
            free(mime);
            mime = xstrdup(s);
        }
    }
    if (m)
        magic_close(m);
    lseek(fd, 0, SEEK_SET);
    badd(&b, "MIME: %s\n\n", mime);
    int got = 0;
    if (!hex_only) {
#ifdef CCURATE_MEDIA
        if (!strcmp(mime, "application/pdf"))
            got = pdf_preview(fd, page, p, &b);
        else if (!strcmp(mime, "image/jpeg"))
            got = preview_jpeg(fd, p, &b);
        else if (!strncmp(mime, "image/", 6))
            got = image_preview(fd, p, &b);
        else if (!strncmp(mime, "video/", 6) || !strncmp(mime, "audio/", 6))
            got = video_preview(fd, page, p, &b);
#endif
        if (!got && !p->rgba)
            got = archive_preview(fd, &b);
    }
    if (!got && !p->rgba) {
        if (!hex_only && !strncmp(mime, "text/", 5)) {
            char text[8193];
            ssize_t n = pread(fd, text, sizeof(text) - 1, 0);
            if (n >= 0) {
                text[n] = 0;
                char *v =
                    display_path(text); /* Keep line breaks for text, escape binary controls. */
                for (ssize_t i = 0; i < n; i++)
                    if (text[i] == '\n' || text[i] == '\t' || text[i] == '\r' ||
                        (unsigned char)text[i] >= 32)
                        badd(&b, "%c", text[i]);
                free(v);
                badd(&b, "\n\n--- HEX / ASCII ---\n");
            }
        }
        hex_preview(fd, &b);
    }
    free(mime);
    p->text = b.s;
    return 0;
}

#ifndef _WIN32
typedef struct {
    uint32_t magic, w, h, pages, text_bytes, pixel_bytes, error;
} Packet;
static int send_preview(Preview *p)
{
    if (p->text && strlen(p->text) > 900000) {
        p->text[900000] = 0;
    }
    Packet h = {0x52435032,
                (uint32_t)p->width,
                (uint32_t)p->height,
                (uint32_t)p->pages,
                (uint32_t)strlen(p->text ? p->text : ""),
                (uint32_t)((size_t)p->width * p->height * 4),
                (uint32_t)p->error};
    return write_all(1, &h, sizeof(h)) || write_all(1, p->text ? p->text : "", h.text_bytes) ||
           (h.pixel_bytes && write_all(1, p->rgba, h.pixel_bytes));
}
static void deny_network(void)
{
    struct sock_filter filter[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
#ifdef __NR_socket
        /* Bubblewrap uses a Unix ioctl socket for if_nametoindex(), then
         * NETLINK_ROUTE to configure loopback inside its private namespace.
         * Permit those local mechanisms; IPv4/IPv6 and connect stay blocked. */
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_socket, 0, 7),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, args[0])),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AF_UNIX, 3, 0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AF_NETLINK, 0, 3),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, args[2])),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, NETLINK_ROUTE, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
#endif
#ifdef __NR_connect
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_connect, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
#endif
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW)};
    struct sock_fprog prog = {(unsigned short)(sizeof(filter) / sizeof(filter[0])), filter};
    prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
    prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog);
}
int preview_worker_main(int fd, int page, int hex_only)
{
    struct rlimit mem = {1024ULL * 1024 * 1024, 1024ULL * 1024 * 1024}, cpu = {12, 14},
                  files = {512ULL * 1024 * 1024, 512ULL * 1024 * 1024}, core = {0, 0};
    /* RLIMIT_FSIZE also covers anonymous memfd buffers. GdkPixbuf's Glycin
     * backend writes the input to a temporary file and shares decoded pixels
     * through memfds. A zero-byte limit kills even tiny, valid images with
     * SIGXFSZ. Bound scratch buffers independently of the 5 MiB output packet;
     * the source descriptor remains read-only. */
    if (setrlimit(RLIMIT_AS, &mem) || setrlimit(RLIMIT_CPU, &cpu) ||
        setrlimit(RLIMIT_FSIZE, &files) || setrlimit(RLIMIT_CORE, &core)) {
        Preview p = {.error = 1,
                     .text = fmt("Preview blocked: cannot configure worker resource limits: %s",
                                 strerror(errno))};
        send_preview(&p);
        preview_free(&p);
        return 1;
    }
    alarm(18);
    if (geteuid() == 0) {
        gid_t gid = 65534;
        uid_t uid = 65534;
        const char *u = getenv("SUDO_UID"), *g = getenv("SUDO_GID");
        if (u && strtoul(u, NULL, 10) > 0)
            uid = (uid_t)strtoul(u, NULL, 10);
        if (g && strtoul(g, NULL, 10) > 0)
            gid = (gid_t)strtoul(g, NULL, 10);
        if (setgroups(0, NULL) || setgid(gid) || setuid(uid) || geteuid() == 0) {
            Preview p = {
                .error = 1,
                .text = xstrdup("Preview blocked: the system refused to drop root privileges.")};
            send_preview(&p);
            preview_free(&p);
            return 1;
        }
    }
    deny_network();
    Preview p;
    preview_read(fd, page, hex_only, &p);
    int rc = send_preview(&p);
    preview_free(&p);
    return rc;
}
int preview_spawn(int fd, int page, int hex_only, Preview *p, atomic_int *cancel)
{
    memset(p, 0, sizeof(*p));
    int output[2] = {-1, -1}, diagnostic[2] = {-1, -1};
    int source = -1, out = -1, err = -1;
    char reason[256] = {0};
    if (pipe2(output, O_CLOEXEC) || pipe2(diagnostic, O_CLOEXEC) ||
        (source = fcntl(fd, F_DUPFD_CLOEXEC, 10)) < 0 ||
        (out = fcntl(output[1], F_DUPFD_CLOEXEC, 10)) < 0 ||
        (err = fcntl(diagnostic[1], F_DUPFD_CLOEXEC, 10)) < 0) {
        snprintf(reason, sizeof(reason), "Cannot prepare preview pipes: %s", strerror(errno));
        goto spawn_failure;
    }
    char pg[20], hx[8];
    snprintf(pg, sizeof(pg), "%d", page);
    snprintf(hx, sizeof(hx), "%d", hex_only);
    char *argv[] = {"ccurate", "--preview-worker", "3", pg, hx, NULL};
    posix_spawn_file_actions_t a;
    posix_spawn_file_actions_init(&a);
    posix_spawn_file_actions_adddup2(&a, source, 3);
    posix_spawn_file_actions_adddup2(&a, out, 1);
    posix_spawn_file_actions_adddup2(&a, err, 2);
    pid_t pid;
    int rc = posix_spawn(&pid, "/proc/self/exe", &a, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&a);
    close(source);
    source = -1;
    close(out);
    out = -1;
    close(err);
    err = -1;
    close(output[1]);
    output[1] = -1;
    close(diagnostic[1]);
    diagnostic[1] = -1;
    if (rc) {
        snprintf(reason, sizeof(reason), "Cannot start preview worker: %s", strerror(rc));
        goto spawn_failure;
    }
    unsigned char *buf = NULL;
    size_t total = 0, cap = 0;
    char stderr_text[32769] = {0};
    size_t stderr_bytes = 0;
    int64_t started = monotonic_ns();
    int cancelled = 0, failed = 0, ended[2] = {0};
    while (!ended[0] || !ended[1]) {
        if (cancel && atomic_load(cancel)) {
            cancelled = 1;
            break;
        }
        if (monotonic_ns() - started > 20000000000LL) {
            snprintf(reason, sizeof(reason), "Preview timed out after 20 seconds");
            failed = 1;
            break;
        }
        struct pollfd pollers[2] = {{ended[0] ? -1 : output[0], POLLIN, 0},
                                    {ended[1] ? -1 : diagnostic[0], POLLIN, 0}};
        int ready = poll(pollers, 2, 100);
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready < 0) {
            snprintf(reason, sizeof(reason), "Preview pipe poll failed: %s", strerror(errno));
            failed = 1;
            break;
        }
        for (int i = 0; i < 2; i++) {
            if (!pollers[i].revents)
                continue;
            unsigned char chunk[65536];
            ssize_t n = read(pollers[i].fd, chunk, sizeof(chunk));
            if (n < 0 && errno == EINTR)
                continue;
            if (n < 0) {
                snprintf(reason, sizeof(reason), "Preview pipe read failed: %s", strerror(errno));
                failed = 1;
                break;
            }
            if (!n) {
                ended[i] = 1;
                continue;
            }
            if (i) {
                size_t keep = (size_t)n;
                if (keep > sizeof(stderr_text) - 1 - stderr_bytes)
                    keep = sizeof(stderr_text) - 1 - stderr_bytes;
                memcpy(stderr_text + stderr_bytes, chunk, keep);
                stderr_bytes += keep;
            } else {
                if (total + (size_t)n > 5 * 1024 * 1024) {
                    snprintf(reason, sizeof(reason), "Preview output exceeded the 5 MiB limit");
                    failed = 1;
                    break;
                }
                if (total + (size_t)n > cap) {
                    cap = total + n + 65536;
                    buf = realloc(buf, cap);
                    if (!buf)
                        abort();
                }
                memcpy(buf + total, chunk, n);
                total += n;
            }
        }
        if (failed)
            break;
    }
    close(output[0]);
    close(diagnostic[0]);
    if (failed || cancelled)
        kill(pid, SIGKILL);
    int status = 0;
    pid_t waited;
    do {
        waited = waitpid(pid, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (cancelled) {
        free(buf);
        p->text = xstrdup("Preview cancelled by a newer selection.");
        return 1;
    }
    if (!failed && total >= sizeof(Packet)) {
        Packet h;
        memcpy(&h, buf, sizeof(h));
        if (h.magic == 0x52435032 && h.w <= 1000 && h.h <= 800 && h.pixel_bytes == h.w * h.h * 4 &&
            h.text_bytes < 1024 * 1024 && total == sizeof(h) + h.text_bytes + h.pixel_bytes) {
            p->width = h.w;
            p->height = h.h;
            p->pages = h.pages;
            p->error = h.error != 0;
            p->text = xmalloc(h.text_bytes + 1);
            memcpy(p->text, buf + sizeof(h), h.text_bytes);
            if (p->error && stderr_bytes) {
                char *safe = display_path(stderr_text);
                char *details = fmt("%s\nDecoder diagnostics: %s", p->text, safe);
                free(safe);
                free(p->text);
                p->text = details;
            }
            if (h.pixel_bytes) {
                p->rgba = xmalloc(h.pixel_bytes);
                memcpy(p->rgba, buf + sizeof(h) + h.text_bytes, h.pixel_bytes);
            }
            free(buf);
            return p->error ? -1 : 0;
        }
        snprintf(reason, sizeof(reason),
                 "Invalid preview packet (dimensions %u x %u, received %zu bytes)", h.w, h.h,
                 total);
    } else if (!failed) {
        if (waited < 0)
            snprintf(reason, sizeof(reason), "Cannot collect preview worker: %s", strerror(errno));
        else if (WIFSIGNALED(status)) {
            int sig = WTERMSIG(status);
            if (sig == SIGALRM || sig == SIGXCPU)
                snprintf(reason, sizeof(reason), "Preview worker exceeded its %s limit",
                         sig == SIGALRM ? "18-second wall-time" : "12-second CPU-time");
            else if (sig == SIGXFSZ)
                snprintf(reason, sizeof(reason),
                         "Preview worker exceeded its 512 MiB temporary-file/shared-memory limit");
            else
                snprintf(reason, sizeof(reason), "Preview worker terminated by signal %d (%s)", sig,
                         strsignal(sig));
        } else
            snprintf(reason, sizeof(reason),
                     "Preview worker exited with status %d without a complete result",
                     WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    }
    free(buf);
    p->error = 1;
    char *safe = display_path(stderr_text);
    p->text = fmt("Preview unavailable: %s.%s%s", reason,
                  stderr_bytes ? "\nDecoder diagnostics: " : "", stderr_bytes ? safe : "");
    free(safe);
    return -1;
spawn_failure:
    if (source >= 0)
        close(source);
    if (out >= 0)
        close(out);
    if (err >= 0)
        close(err);
    for (int i = 0; i < 2; i++) {
        if (output[i] >= 0)
            close(output[i]);
        if (diagnostic[i] >= 0)
            close(diagnostic[i]);
    }
    p->error = 1;
    p->text = xstrdup(reason);
    return -1;
}
#endif
