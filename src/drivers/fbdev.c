#include <string.h>
#include "drivers/vbe.h"
#include "drivers/fbdev.h"
#include "kernel/kernel.h"
#include "fonts/vga8x16.h"

#define MODULE "fbdev: "

struct fbdev_console {
    char *framebuffer;
    uint32_t height;
    uint32_t width;
    uint8_t depth;
    uint16_t pitch;
    uint32_t cols;
    uint32_t rows;
    uint32_t cur_col;
    uint32_t cur_row;
    char cells[FBDEV_MAX_COLS * FBDEV_MAX_ROWS];
};

static struct fbdev_console console;
static bool fbdev_ready = false;

bool fbdev_is_ready(void)
{
    return fbdev_ready;
}

static void putpixel(uint32_t x, uint32_t y, uint32_t color)
{
    unsigned where = (x * (console.depth / 8)) + (y * console.pitch);
    console.framebuffer[where] = color & 255;              // blue
    console.framebuffer[where + 1] = (color >> 8) & 255;   // green
    console.framebuffer[where + 2] = (color >> 16) & 255;  // red
}

static void clear_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    for (uint32_t row = y; row < y + h; row++) {
        unsigned where = (x * (console.depth / 8)) + (row * console.pitch);
        memset(&console.framebuffer[where], 0, w * (console.depth / 8));
    }
}

static void drawglyph(uint8_t *glyph, uint32_t x, uint32_t y, uint32_t fgcolor)
{
    uint32_t b = 0;
    for (uint32_t j = 0; j < CONSOLE_FONT_HEIGHT; j++) {
        for (uint32_t i = 0; i < CONSOLE_FONT_WIDTH; i++) {
            uint32_t val = (*glyph << b) & 0x80;
            if (val)
                putpixel(x+i, y+j, fgcolor);
            b++;
            if (b == 8) {
                glyph++;
                b = 0;
            }
        }
    }
}

static void drawchar(uint8_t c, uint32_t x, uint32_t y, uint32_t fgcolor)
{
    uint32_t g = ((uint32_t)c) * CONSOLE_FONT_WIDTH * CONSOLE_FONT_HEIGHT / 8;
    drawglyph(&fontdata[g], x, y, fgcolor);
}

/* Redraw the entire screen from the character back-buffer */
static void redraw_all(void)
{
    uint32_t fgcolor = rgb(255, 255, 255);

    /* Clear the whole framebuffer */
    for (uint32_t y = 0; y < console.height; y++) {
        unsigned where = y * console.pitch;
        memset(&console.framebuffer[where], 0, console.width * (console.depth / 8));
    }

    /* Redraw every character from the back-buffer */
    for (uint32_t row = 0; row < console.rows; row++) {
        for (uint32_t col = 0; col < console.cols; col++) {
            char c = console.cells[row * console.cols + col];
            if (c && c != ' ') {
                drawchar(c, col * CONSOLE_FONT_WIDTH, row * CONSOLE_FONT_HEIGHT, fgcolor);
            }
        }
    }
}

static void scroll(void)
{
    /* Shift the back-buffer up by one row */
    uint32_t row_bytes = console.cols;
    memcpy(&console.cells[0], &console.cells[row_bytes],
            row_bytes * (console.rows - 1));

    /* Clear the last row in the back-buffer */
    memset(&console.cells[(console.rows - 1) * row_bytes], 0, row_bytes);

    redraw_all();
}

void writechar_fb(char c)
{
    if (!fbdev_ready)
        return;

    if (c == '\n') {
        console.cur_col = 0;
        console.cur_row++;
    } else if (c == '\r') {
        console.cur_col = 0;
    } else if (c == '\b') {
        if (console.cur_col > 0)
            console.cur_col--;
    } else if (c == '\t') {
        console.cur_col = (console.cur_col + 4) & ~3;
    } else {
        /* Wrap if we hit the right edge */
        if (console.cur_col >= console.cols) {
            console.cur_col = 0;
            console.cur_row++;
        }

        /* Scroll if we hit the bottom */
        if (console.cur_row >= console.rows)  {
            scroll();
            console.cur_row = console.rows - 1;
        }

        /* Write to back-buffer and draw */
        console.cells[console.cur_row * console.cols + console.cur_col] = c;
        uint32_t fgcolor = rgb(255, 255, 255);
        drawchar(c, console.cur_col * CONSOLE_FONT_WIDTH,
                console.cur_row * CONSOLE_FONT_HEIGHT, fgcolor);
        console.cur_col++;
        return;
    }

    /* Handle scroll after newline/wrap */
    if (console.cur_row >= console.rows) {
        scroll();
        console.cur_row = console.rows - 1;
    }
}

void writestr_fb(char *str)
{
    while (*str)
        writechar_fb(*str++);
}

void writeline_fb(uint32_t row, char *str, uint32_t fgcolor, uint32_t bgcolor)
{
    if (!fbdev_ready || row >= console.rows)
        return;

    /* Clear the row on screen */
    clear_rect(0, row * CONSOLE_FONT_HEIGHT,
            console.width, CONSOLE_FONT_HEIGHT);

    /* Draw each character */
    uint32_t col = 0;
    while (*str && col < console.cols) {
        drawchar(*str++, col * CONSOLE_FONT_WIDTH,
                row * CONSOLE_FONT_HEIGHT, fgcolor);
        col++;
    }
}

void init_fbdev(char *fbaddr, uint32_t width, uint32_t height, uint8_t depth,
        uint16_t pitch)
{
    console.framebuffer = fbaddr;
    console.height = height;
    console.width = width;
    console.depth = depth;
    console.pitch = pitch;
    console.cols = width / CONSOLE_FONT_WIDTH;
    console.rows = height / CONSOLE_FONT_HEIGHT;
    console.cur_col = 0;
    console.cur_row = 0;

    /* Cap to max buffer size */
    if (console.cols > FBDEV_MAX_COLS)
        console.cols = FBDEV_MAX_COLS;
    if (console.rows > FBDEV_MAX_ROWS)
        console.rows = FBDEV_MAX_ROWS;

    /* Clear the back-buffer */
    memset(console.cells, 0, sizeof(console.cells));

    fbdev_ready = true;
    printk(MODULE "framebuffer console %dx%d (%d cols x %d rows)\n",
            width, height, console.cols, console.rows);
}
