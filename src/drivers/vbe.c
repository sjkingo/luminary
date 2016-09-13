#include "boot/multiboot.h"
#include "drivers/vbe.h"
#include "kernel/kernel.h"
#include "fonts/console.h"
#include "drivers/fbdev.h"

#define MODULE "vbe: "

static uint8_t *font = (uint8_t*)fontdata;
static struct vbe_control_struct *control = NULL;
static struct vbe_mode_info_struct *mode = NULL;

uint32_t rgb(uint8_t r, uint8_t g, uint8_t b) {
	return 0xFF000000 + (r * 0x10000) + (g * 0x100) + (b * 0x1);
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

    // Create the virtual framebuffer
    init_fbdev((char *)mode->framebuffer, mode->width, mode->height,
        mode->bpp, mode->pitch);
    //drawstr("Hello, world! This is Luminary.", 0, 0, rgb(255,255,255));
    goto success;

success:
    return;
fail:
    control = NULL;
    mode = NULL;
    printk(MODULE "video mode init failed!\n");
    return;
}
