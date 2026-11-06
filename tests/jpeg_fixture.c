/* Streaming generator: large fixtures without a full RGB input allocation. */
#include <stdio.h>
#include <stdlib.h>

#include <jpeglib.h>

int main(int argc, char **argv)
{
    if (argc != 5)
        return 2;
    int width = atoi(argv[2]), height = atoi(argv[3]);
    if (width < 1 || height < 1 || width > 65000 || height > 65000)
        return 2;
    FILE *file = fopen(argv[1], "wb");
    unsigned char *row = malloc((size_t)width * 3);
    if (!file || !row)
        return 1;
    struct jpeg_compress_struct c;
    struct jpeg_error_mgr error;
    c.err = jpeg_std_error(&error);
    jpeg_create_compress(&c);
    jpeg_stdio_dest(&c, file);
    c.image_width = width;
    c.image_height = height;
    c.input_components = 3;
    c.in_color_space = JCS_RGB;
    jpeg_set_defaults(&c);
    jpeg_set_quality(&c, 85, TRUE);
    if (atoi(argv[4]))
        jpeg_simple_progression(&c);
    jpeg_start_compress(&c, TRUE);
    while (c.next_scanline < c.image_height) {
        for (int x = 0; x < width; x++) {
            row[3 * x] = x < width / 2 ? 225 : 25;
            row[3 * x + 1] = c.next_scanline < c.image_height / 2 ? 25 : 225;
            row[3 * x + 2] = 45;
        }
        JSAMPROW scanline = row;
        jpeg_write_scanlines(&c, &scanline, 1);
    }
    jpeg_finish_compress(&c);
    jpeg_destroy_compress(&c);
    free(row);
    return fclose(file) ? 1 : 0;
}
