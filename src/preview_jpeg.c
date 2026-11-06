#include "preview_jpeg.h"
#include <errno.h>
#include <jpeglib.h>
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    struct jpeg_decompress_struct decoder;
    struct jpeg_error_mgr errors;
    jmp_buf escape;
    FILE *input;
    unsigned char *row;
    int created;
    char message[JMSG_LENGTH_MAX];
} Jpeg;

static void jpeg_failed(j_common_ptr decoder)
{
    Jpeg *jpeg = decoder->client_data;
    decoder->err->format_message(decoder, jpeg->message);
    longjmp(jpeg->escape, 1);
}

static unsigned read16(const unsigned char *p, int little)
{
    return little ? p[0] | ((unsigned)p[1] << 8) : ((unsigned)p[0] << 8) | p[1];
}

static uint32_t read32(const unsigned char *p, int little)
{
    return little ? read16(p, 1) | ((uint32_t)read16(p + 2, 1) << 16)
                  : ((uint32_t)read16(p, 0) << 16) | read16(p + 2, 0);
}

static int exif_orientation(const unsigned char *p, size_t size)
{
    if (size < 14 || memcmp(p, "Exif\0\0", 6))
        return 0;
    p += 6;
    size -= 6;
    int little = !memcmp(p, "II", 2);
    if ((!little && memcmp(p, "MM", 2)) || read16(p + 2, little) != 42)
        return 0;
    uint32_t offset = read32(p + 4, little);
    if (offset > size - 2)
        return 0;
    unsigned count = read16(p + offset, little);
    size_t at = (size_t)offset + 2;
    for (unsigned i = 0; i < count && at <= size && size - at >= 12; i++, at += 12)
        if (read16(p + at, little) == 0x112 && read16(p + at + 2, little) == 3 &&
            read32(p + at + 4, little) == 1) {
            unsigned value = read16(p + at + 8, little);
            return value >= 1 && value <= 8 ? (int)value : 0;
        }
    return 0;
}

static int source_orientation(int fd)
{
    unsigned char marker[4], data[65533];
    off_t at = 2;
    /* Read marker payloads individually, never accumulate EXIF/ICC metadata. */
    while (pread(fd, marker, 2, at) == 2 && marker[0] == 0xff) {
        if (marker[1] == 0xff) {
            at++;
            continue;
        }
        if (marker[1] == 0xda || marker[1] == 0xd9)
            break;
        if (marker[1] == 0x01 || (marker[1] >= 0xd0 && marker[1] <= 0xd8)) {
            at += 2;
            continue;
        }
        if (pread(fd, marker + 2, 2, at + 2) != 2)
            break;
        unsigned length = read16(marker + 2, 0);
        if (length < 2)
            break;
        if (marker[1] == 0xe1 && pread(fd, data, length - 2, at + 4) == (ssize_t)length - 2) {
            int orientation = exif_orientation(data, length - 2);
            if (orientation)
                return orientation;
        }
        at += length + 2;
    }
    return 1;
}

static size_t oriented_offset(int x, int y, int w, int h, int orientation)
{
    int dx = x, dy = y, stride = orientation >= 5 ? h : w;
    switch (orientation) {
    case 2:
        dx = w - 1 - x;
        break;
    case 3:
        dx = w - 1 - x;
        dy = h - 1 - y;
        break;
    case 4:
        dy = h - 1 - y;
        break;
    case 5:
        dx = y;
        dy = x;
        break;
    case 6:
        dx = h - 1 - y;
        dy = x;
        break;
    case 7:
        dx = h - 1 - y;
        dy = w - 1 - x;
        break;
    case 8:
        dx = y;
        dy = w - 1 - x;
        break;
    }
    return ((size_t)dy * stride + dx) * 4;
}

int preview_jpeg(int fd, Preview *p, Buf *details)
{
    fprintf(stderr, "Image preview backend: libjpeg (scanline thumbnail)\n");
    struct stat st;
    if (fstat(fd, &st)) {
        p->error = 1;
        badd(details, "Image decoder: cannot inspect JPEG input: %s\n", strerror(errno));
        return 0;
    }
    if (st.st_size > 256LL * 1024 * 1024) {
        p->error = 1;
        badd(details, "Image decoder: JPEG input exceeds the 256 MiB preview limit.\n");
        return 0;
    }
    Jpeg *jpeg = calloc(1, sizeof(*jpeg));
    if (!jpeg) {
        p->error = 1;
        badd(details, "Image decoder: cannot allocate JPEG state.\n");
        return 0;
    }
    jpeg->decoder.err = jpeg_std_error(&jpeg->errors);
    jpeg->errors.error_exit = jpeg_failed;
    jpeg->decoder.client_data = jpeg;
    if (setjmp(jpeg->escape)) {
        badd(details, "Image decoder: %s\n", jpeg->message);
        p->error = 1;
        free(p->rgba);
        p->rgba = NULL;
        p->width = p->height = 0;
        goto done;
    }
    int copy = dup(fd);
    jpeg->input = copy >= 0 ? fdopen(copy, "rb") : NULL;
    if (!jpeg->input) {
        if (copy >= 0)
            close(copy);
        snprintf(jpeg->message, sizeof(jpeg->message), "Cannot read JPEG: %s", strerror(errno));
        longjmp(jpeg->escape, 1);
    }
    rewind(jpeg->input);
    jpeg_create_decompress(&jpeg->decoder);
    jpeg->created = 1;
    jpeg_stdio_src(&jpeg->decoder, jpeg->input);
    jpeg_read_header(&jpeg->decoder, TRUE);
    struct jpeg_decompress_struct *d = &jpeg->decoder;
    int orientation = source_orientation(fd), swap = orientation >= 5;
    int original_w = (int)d->image_width, original_h = (int)d->image_height;
    int oriented_w = swap ? original_h : original_w, oriented_h = swap ? original_w : original_h;
    double scale = 1.0;
    if (oriented_w > 1000)
        scale = 1000.0 / oriented_w;
    if (oriented_h * scale > 800)
        scale = 800.0 / oriented_h;
    p->width = (int)(oriented_w * scale);
    p->height = (int)(oriented_h * scale);
    if (p->width < 1)
        p->width = 1;
    if (p->height < 1)
        p->height = 1;
    int w = swap ? p->height : p->width, h = swap ? p->width : p->height;
    int cmyk = d->jpeg_color_space == JCS_CMYK || d->jpeg_color_space == JCS_YCCK;
    d->out_color_space = cmyk ? JCS_CMYK : JCS_RGB;
    d->scale_num = 1;
    d->scale_denom = 1;
    for (int denominator = 2; denominator <= 8; denominator *= 2)
        if ((d->image_width + denominator - 1) / denominator >= (unsigned)w &&
            (d->image_height + denominator - 1) / denominator >= (unsigned)h)
            d->scale_denom = denominator;
    /* Progressive data needs full coefficient arrays even with IDCT scaling.
     * Bound that independently of libjpeg implementations' memory-manager policy. */
    uint64_t coefficients = 0;
    if (jpeg_has_multiple_scans(d))
        for (int i = 0; i < d->num_components; i++) {
            jpeg_component_info *component = d->comp_info + i;
            uint64_t cols = ((uint64_t)d->image_width * component->h_samp_factor +
                             d->max_h_samp_factor * DCTSIZE - 1) /
                            (d->max_h_samp_factor * DCTSIZE);
            uint64_t rows = ((uint64_t)d->image_height * component->v_samp_factor +
                             d->max_v_samp_factor * DCTSIZE - 1) /
                            (d->max_v_samp_factor * DCTSIZE);
            coefficients += cols * rows * sizeof(JBLOCK);
        }
    if (coefficients > 512ULL * 1024 * 1024) {
        snprintf(jpeg->message, sizeof(jpeg->message),
                 "Progressive JPEG coefficient data exceeds the 512 MiB preview budget");
        longjmp(jpeg->escape, 1);
    }
    d->mem->max_memory_to_use = 512L * 1024 * 1024;
    jpeg_start_decompress(d);
    jpeg->row = malloc((size_t)d->output_width * d->output_components);
    p->rgba = malloc((size_t)p->width * p->height * 4);
    if (!jpeg->row || !p->rgba) {
        snprintf(jpeg->message, sizeof(jpeg->message), "Cannot allocate JPEG thumbnail buffers");
        longjmp(jpeg->escape, 1);
    }
    int target_y = 0;
    while (d->output_scanline < d->output_height) {
        unsigned y = d->output_scanline;
        JSAMPROW row = jpeg->row;
        jpeg_read_scanlines(d, &row, 1);
        if (target_y >= h ||
            y != (unsigned)(((uint64_t)target_y * 2 + 1) * d->output_height / (2 * h)))
            continue;
        for (int x = 0; x < w; x++) {
            unsigned source_x = (unsigned)(((uint64_t)x * 2 + 1) * d->output_width / (2 * w));
            const unsigned char *sample = row + (size_t)source_x * d->output_components;
            uint8_t *pixel = p->rgba + oriented_offset(x, target_y, w, h, orientation);
            for (int channel = 0; channel < 3; channel++)
                pixel[channel] =
                    !cmyk ? sample[channel]
                    : d->saw_Adobe_marker
                        ? ((unsigned)sample[channel] * sample[3] + 127) / 255
                        : ((255 - (unsigned)sample[channel]) * (255 - sample[3]) + 127) / 255;
            pixel[3] = 255;
        }
        target_y++;
    }
    jpeg_finish_decompress(d);
    badd(details,
         "Original image: %d x %d px\nImage: oriented preview %d x %d px\n"
         "Decoder: libjpeg (IDCT 1/%u, scanline thumbnail)\norientation: %d\n",
         original_w, original_h, p->width, p->height, d->scale_denom, orientation);
    if (d->density_unit)
        badd(details, "x-dpi: %.2f\ny-dpi: %.2f\n",
             d->X_density * (d->density_unit == 2 ? 2.54 : 1),
             d->Y_density * (d->density_unit == 2 ? 2.54 : 1));
done:
    if (jpeg->created)
        jpeg_destroy_decompress(&jpeg->decoder);
    if (jpeg->input)
        fclose(jpeg->input);
    free(jpeg->row);
    free(jpeg);
    return p->rgba != NULL;
}
