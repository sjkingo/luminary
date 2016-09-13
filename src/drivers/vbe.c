#include "boot/multiboot.h"
#include "drivers/vbe.h"
#include "kernel/kernel.h"
#include "fonts/console.h"

#define MODULE "vbe: "

uint8_t *font = (uint8_t*)fontdata;

struct vbe_control_struct *control = NULL;
struct vbe_mode_info_struct *mode = NULL;

uint32_t rgb(uint8_t r, uint8_t g, uint8_t b) {
	return 0xFF000000 + (r * 0x10000) + (g * 0x100) + (b * 0x1);
}

static void putpixel(uint32_t x, uint32_t y, uint32_t color)
{
    uint8_t *fb = (uint8_t *)mode->framebuffer;
    unsigned where = (x * (mode->bpp / 8)) + (y * mode->pitch);
    fb[where] = color & 255;              // blue
    fb[where + 1] = (color >> 8) & 255;   // green
    fb[where + 2] = (color >> 16) & 255;  // red
}

static void dump_vbe_structs(void)
{
    printk(MODULE "found valid VESA extensions (version %d.%d):\n",
        control->major_version, control->minor_version);
    printk("  framebuffer 0x%x\n", mode->framebuffer);
    printk("  memory %d KB\n", control->video_memory*64);
    printk("  current mode %dx%dx%d (pitch=%d)\n", mode->width,
        mode->height, mode->bpp, mode->pitch);
}

/* Oh my so inefficient... */
static void fill_screen(uint32_t color)
{
    for (int x = 0; x < mode->width; x++) {
        for (int y = 0; y < mode->height; y++) {
            putpixel(x, y, color);
        }
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
    drawglyph(&font[g], x, y, fgcolor);
}

static void drawstr(char *str, uint32_t start_x, uint32_t start_y, uint32_t fgcolor)
{
    uint32_t x = start_x;
    uint32_t y = start_y;
    while (*str) {
        drawchar(*str++, x, y, fgcolor);
        x += CONSOLE_FONT_WIDTH;
    }
}

void init_vbe(struct multiboot_info *mb)
{
    control = mb->vbe_control_info;
    mode = mb->vbe_mode_info;

    /* Check signature to ensure VBE/VESA is enabled */
    if (! (control->signature[0] == 'V' && control->signature[1] == 'E' &&
            control->signature[2] == 'S' && control->signature[3] == 'A')) {
        printk(MODULE "no VBE structures found in Multiboot header, disabling VESA\n");
        goto fail;
    }

    /* Check if the video mode supports linear frame buffer (LFB, bit 7) */
    if (!(mode->attributes & (1 << 7))) {
        printk(MODULE "warning: video mode does not support LFB\n");
        goto fail;
    }

    dump_vbe_structs();
    drawstr("Hello, world! This is Luminary.", 0, 0, rgb(255,255,255));
    goto success;

success:
    return;
fail:
    control = NULL;
    mode = NULL;
    printk(MODULE "video mode init failed!\n");
    return;
}
