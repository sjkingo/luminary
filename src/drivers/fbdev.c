/* fbdev.c — minimal kernel framebuffer console for printk and panic output.
 *
 * This is intentionally a dumb line-wrapped console: it tracks cur_col and
 * cur_row and writes characters directly to the framebuffer.  There is no
 * scrollback ring, no scrollback navigation, and no cursor-movement support.
 *
 * Full console features (scrollback, line editing, history) live in the
 * userland fbcon daemon which renders via /dev/fb0 and reads /dev/console.
 */

#include <string.h>
#include "drivers/vbe.h"
#include "drivers/fbdev.h"
#include "kernel/kernel.h"
#include "fonts/ctrld16r.h"

/* Solarized Dark palette */
#define SOL_BASE03  rgb( 30,  30,  50)   /* background */
#define SOL_BASE0   rgb(147, 161, 161)   /* foreground */

struct fbdev_console {
    char    *framebuffer;
    uint32_t height;
    uint32_t width;
    uint8_t  depth;
    uint16_t pitch;
    uint32_t cols;
    uint32_t rows;
    uint32_t cur_col;
    uint32_t cur_row;
};

static struct fbdev_console console;
static bool fbdev_ready = false;

bool fbdev_is_ready(void)
{
    return fbdev_ready;
}

static void putpixel(uint32_t x, uint32_t y, uint32_t color)
{
    if (x >= console.width || y >= console.height)
        return;
    unsigned where = (x * (console.depth / 8)) + (y * console.pitch);
    console.framebuffer[where]     = color & 255;
    console.framebuffer[where + 1] = (color >> 8) & 255;
    console.framebuffer[where + 2] = (color >> 16) & 255;
}

static void clear_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    uint32_t color = SOL_BASE03;
    uint8_t b0 = color & 0xFF;
    uint8_t b1 = (color >> 8) & 0xFF;
    uint8_t b2 = (color >> 16) & 0xFF;
    uint32_t bpp = console.depth / 8;

    if (x >= console.width || y >= console.height) return;
    if (x + w > console.width)  w = console.width - x;
    if (y + h > console.height) h = console.height - y;

    for (uint32_t row = y; row < y + h; row++) {
        unsigned base = (x * bpp) + (row * console.pitch);
        for (uint32_t col = 0; col < w; col++) {
            unsigned where = base + col * bpp;
            console.framebuffer[where]     = b0;
            console.framebuffer[where + 1] = b1;
            console.framebuffer[where + 2] = b2;
        }
    }
}

static void drawglyph(const uint8_t *glyph, uint32_t x, uint32_t y, uint32_t fgcolor)
{
    uint32_t b = 0;
    for (uint32_t j = 0; j < CONSOLE_FONT_HEIGHT; j++) {
        for (uint32_t i = 0; i < CONSOLE_FONT_WIDTH; i++) {
            if ((*glyph << b) & 0x80)
                putpixel(x + i, y + j, fgcolor);
            if (++b == 8) { glyph++; b = 0; }
        }
    }
}

static void drawchar(uint8_t c, uint32_t x, uint32_t y, uint32_t fgcolor)
{
    uint32_t g = ((uint32_t)c) * CONSOLE_FONT_WIDTH * CONSOLE_FONT_HEIGHT / 8;
    drawglyph(&fontdata[g], x, y, fgcolor);
}

static void scroll_up_one(void)
{
    uint32_t row_bytes = (uint32_t)CONSOLE_FONT_HEIGHT * console.pitch;
    char *fb = console.framebuffer;
    memcpy(fb, fb + row_bytes, row_bytes * (console.rows - 1));
    clear_rect(0, (console.rows - 1) * CONSOLE_FONT_HEIGHT,
               console.width, CONSOLE_FONT_HEIGHT);
}

void writechar_fb(char c)
{
    if (!fbdev_ready)
        return;

    if (c == '\n') {
        console.cur_col = 0;
        console.cur_row++;
        if (console.cur_row >= console.rows) {
            scroll_up_one();
            console.cur_row = console.rows - 1;
        }
        return;
    }
    if (c == '\r') {
        console.cur_col = 0;
        return;
    }
    if (c == '\b') {
        if (console.cur_col > 0) {
            console.cur_col--;
            clear_rect(console.cur_col * CONSOLE_FONT_WIDTH,
                       console.cur_row * CONSOLE_FONT_HEIGHT,
                       CONSOLE_FONT_WIDTH, CONSOLE_FONT_HEIGHT);
        }
        return;
    }
    if (c == '\t') {
        console.cur_col = (console.cur_col + 4) & ~3u;
        if (console.cur_col >= console.cols) {
            console.cur_col = 0;
            console.cur_row++;
            if (console.cur_row >= console.rows) {
                scroll_up_one();
                console.cur_row = console.rows - 1;
            }
        }
        return;
    }
    if ((unsigned char)c < 32)
        return;

    if (console.cur_col >= console.cols) {
        console.cur_col = 0;
        console.cur_row++;
        if (console.cur_row >= console.rows) {
            scroll_up_one();
            console.cur_row = console.rows - 1;
        }
    }

    drawchar((uint8_t)c,
             console.cur_col * CONSOLE_FONT_WIDTH,
             console.cur_row * CONSOLE_FONT_HEIGHT,
             SOL_BASE0);
    console.cur_col++;
}

void init_fbdev(char *fbaddr, uint32_t width, uint32_t height, uint8_t depth,
        uint16_t pitch)
{
    console.framebuffer = fbaddr;
    console.height = height;
    console.width  = width;
    console.depth  = depth;
    console.pitch  = pitch;
    console.cols   = width / CONSOLE_FONT_WIDTH;
    console.rows   = height / CONSOLE_FONT_HEIGHT;
    console.cur_col = 0;
    console.cur_row = 0;

    if (console.cols == 0 || console.rows == 0) {
        printk("fbdev: display too small for console (%ldx%ld)\n", width, height);
        return;
    }

    fbdev_ready = true;
    clear_rect(0, 0, console.width, console.height);

    printk("fbdev: framebuffer console %ldx%ld (%ld cols x %ld rows)\n",
            width, height, console.cols, console.rows);
}
