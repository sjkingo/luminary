#include "drivers/vbe.h"
#include "kernel/kernel.h"
#include "fonts/console.h"

#define MODULE "fbdev: "

struct fbdev_console {
    char *framebuffer;
    uint32_t last_x;
    uint32_t last_y;
    uint32_t height;
    uint32_t width;
    uint8_t depth;
    uint16_t pitch;
};

static struct fbdev_console console;

static void putpixel(uint32_t x, uint32_t y, uint32_t color)
{
    unsigned where = (x * (console.depth / 8)) + (y * console.pitch);
    console.framebuffer[where] = color & 255;              // blue
    console.framebuffer[where + 1] = (color >> 8) & 255;   // green
    console.framebuffer[where + 2] = (color >> 16) & 255;  // red
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

void writestr_fb(char *str)
{
    uint32_t fgcolor = rgb(255,255,255);
    while (*str) {
        if (console.last_x + 1 >= console.width || *str == '\n') {
            console.last_x = 0;
            console.last_y += CONSOLE_FONT_HEIGHT;
            if (*str == '\n') {
                str++;
                continue;
            }
        }
        drawchar(*str++, console.last_x, console.last_y, fgcolor);
        console.last_x += CONSOLE_FONT_WIDTH;
    }
}

void writechar_fb(char c)
{
    uint32_t fgcolor = rgb(255,255,255);

    if (console.last_x + 1 >= console.width || c == '\n') {
        console.last_x = 0;
        console.last_y += CONSOLE_FONT_HEIGHT;
        if (c == '\n')
            return;
    }
    drawchar(c, console.last_x, console.last_y, fgcolor);
    console.last_x += CONSOLE_FONT_WIDTH;
}

void init_fbdev(char *fbaddr, uint32_t width, uint32_t height, uint8_t depth,
        uint16_t pitch)
{
    // TODO: hack until kmalloc() exists - write directly into video memory
    console.framebuffer = fbaddr;
    console.last_x = 0;
    console.last_y = 0;
    console.height = height;
    console.width = width;
    console.depth = depth;
    console.pitch = pitch;
    printk("creating framebuffer %dx%d\n", console.width, console.height);
}
