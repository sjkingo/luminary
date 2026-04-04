/* wallpaper — set the GUI desktop background from a BMP file.
 *
 * Usage: wallpaper <file.bmp>
 *
 * Supports 24bpp and 32bpp uncompressed BMP (BI_RGB).
 * The image is scaled to fit the screen (preserving aspect ratio,
 * letterboxed) by the kernel compositor.
 *
 * Memory layout: two static buffers in BSS.
 *   raw_buf  — one BMP row at a time (max row width × 4 bytes)
 *   argb_buf — full converted image (max MAX_W × MAX_H × 4 bytes)
 *
 * Max supported image: 1280×960 (fits a typical VBE framebuffer).
 */

#include "syscall.h"
#include "libc/stdio.h"

#define MAX_W   1280
#define MAX_H   1092

/* BMP file header (14 bytes) */
struct bmp_file_hdr {
    unsigned char  magic[2];
    unsigned int   file_size;
    unsigned short reserved1;
    unsigned short reserved2;
    unsigned int   data_offset;
} __attribute__((packed));

/* BMP DIB header — BITMAPINFOHEADER (40 bytes) */
struct bmp_dib_hdr {
    unsigned int   hdr_size;
    int            width;
    int            height;
    unsigned short planes;
    unsigned short bpp;
    unsigned int   compression;
    unsigned int   image_size;
    int            x_ppm;
    int            y_ppm;
    unsigned int   colors_used;
    unsigned int   colors_important;
} __attribute__((packed));

/* Static BSS buffers — not on the stack */
static unsigned int  argb_buf[MAX_W * MAX_H];
static unsigned char raw_row[MAX_W * 4];  /* max bytes per raw BMP row */

static int read_exact(int fd, void *buf, unsigned int len)
{
    unsigned char *p = (unsigned char *)buf;
    while (len > 0) {
        unsigned int chunk = len > 65536 ? 65536 : len;
        int n = read(fd, (char *)p, chunk);
        if (n <= 0) return -1;
        p   += n;
        len -= (unsigned int)n;
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: wallpaper <file.bmp> [r,g,b]\n");
        exit(1);
    }

    /* Optional second arg: desktop fill colour "r,g,b" */
    if (argc >= 3) {
        const char *s = argv[2];
        unsigned int r = 0, g = 0, b = 0;
        /* parse three decimal numbers separated by commas */
        while (*s >= '0' && *s <= '9') r = r * 10 + (unsigned int)(*s++ - '0');
        if (*s++ != ',') { printf("wallpaper: invalid colour '%s', expected r,g,b\n", argv[2]); exit(1); }
        while (*s >= '0' && *s <= '9') g = g * 10 + (unsigned int)(*s++ - '0');
        if (*s++ != ',') { printf("wallpaper: invalid colour '%s', expected r,g,b\n", argv[2]); exit(1); }
        while (*s >= '0' && *s <= '9') b = b * 10 + (unsigned int)(*s++ - '0');
        if (*s != '\0')  { printf("wallpaper: invalid colour '%s', expected r,g,b\n", argv[2]); exit(1); }
        gui_set_desktop_color(r, g, b);
    }

    int fd = open(argv[1], O_RDONLY);
    if (fd < 0) {
        printf("wallpaper: cannot open '%s'\n", argv[1]);
        exit(1);
    }

    struct bmp_file_hdr fhdr;
    if (read_exact(fd, &fhdr, sizeof(fhdr)) < 0 ||
        fhdr.magic[0] != 'B' || fhdr.magic[1] != 'M') {
        printf("wallpaper: not a BMP file\n");
        vfs_close(fd);
        exit(1);
    }

    struct bmp_dib_hdr dib;
    if (read_exact(fd, &dib, sizeof(dib)) < 0) {
        printf("wallpaper: truncated header\n");
        vfs_close(fd);
        exit(1);
    }

    /* BI_RGB=0, BI_BITFIELDS=3 (explicit channel masks, pixels still uncompressed) */
    if (dib.compression != 0 && dib.compression != 3) {
        printf("wallpaper: compressed BMP not supported (compression=%d)\n",
               dib.compression);
        vfs_close(fd);
        exit(1);
    }
    if (dib.bpp != 24 && dib.bpp != 32) {
        printf("wallpaper: only 24bpp/32bpp supported (got %d)\n", dib.bpp);
        vfs_close(fd);
        exit(1);
    }

    int img_w = dib.width;
    int img_h = dib.height;
    int bottom_up = 1;
    if (img_h < 0) { img_h = -img_h; bottom_up = 0; }

    if (img_w <= 0 || img_h <= 0 || img_w > MAX_W || img_h > MAX_H) {
        printf("wallpaper: image too large or invalid (%dx%d, max %dx%d)\n",
               img_w, img_h, MAX_W, MAX_H);
        vfs_close(fd);
        exit(1);
    }

    unsigned int bpp       = dib.bpp / 8;
    unsigned int row_bytes = (unsigned int)img_w * bpp;
    /* BMP rows are padded to 4-byte boundaries */
    unsigned int row_stride = (row_bytes + 3) & ~3u;

    /* Seek to pixel data */
    vfs_lseek(fd, fhdr.data_offset, 0);

    /* Read rows and convert to ARGB, storing in correct display order */
    for (int y = 0; y < img_h; y++) {
        /* BMP bottom-up: row 0 in file = bottom row of image */
        int dst_y = bottom_up ? (img_h - 1 - y) : y;

        if (read_exact(fd, raw_row, row_stride) < 0) {
            printf("wallpaper: unexpected end of file\n");
            vfs_close(fd);
            exit(1);
        }

        unsigned int *dst = argb_buf + (unsigned int)dst_y * (unsigned int)img_w;
        for (int x = 0; x < img_w; x++) {
            unsigned char b = raw_row[x * bpp + 0];
            unsigned char g = raw_row[x * bpp + 1];
            unsigned char r = raw_row[x * bpp + 2];
            dst[x] = 0xFF000000u | ((unsigned int)r << 16) |
                     ((unsigned int)g << 8) | b;
        }
    }

    vfs_close(fd);

    if (gui_set_bg(argb_buf, (unsigned int)img_w, (unsigned int)img_h) < 0)
        printf("wallpaper: gui_set_bg failed\n");

    return 0;
}
