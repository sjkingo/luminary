/* fb_dev.c — /dev/fb0 chardev exposing framebuffer geometry to userland.
 *
 * Userland opens /dev/fb0 and calls ioctl(fd, FBIOGET_INFO, &info) to get
 * the framebuffer physical address and dimensions.  The VBE framebuffer is
 * identity-mapped with PTE_USER so userland can write to it directly.
 */

#include <stdint.h>
#include <string.h>

#include "kernel/fb_dev.h"
#include "kernel/kernel.h"
#include "kernel/vfs.h"
#include "drivers/vbe.h"

static bool user_ptr_ok(const void *ptr, uint32_t len)
{
    uint32_t addr = (uint32_t)ptr;
    if (addr < 0x01000000) return false;
    if (addr >= 0xC0000000) return false;
    if (len > 0 && len > 0xC0000000 - addr) return false;
    return true;
}

static int32_t fb_ioctl_op(struct vfs_node *node, uint32_t request, void *arg)
{
    (void)node;

    if (request == FBIOGET_INFO) {
        struct fb_info *out = (struct fb_info *)arg;
        if (!user_ptr_ok(out, sizeof(struct fb_info)))
            return -1;

        struct vbe_mode_info_struct *mode = vbe_get_mode_info();
        if (!mode)
            return -1;

        out->width   = mode->width;
        out->height  = mode->height;
        out->pitch   = mode->pitch;
        out->depth   = mode->bpp;
        out->fb_addr = mode->framebuffer;
        return 0;
    }

    return -1;
}

void init_fb_dev(void)
{
    if (!vfs_register_dev("fb0", 120, NULL, NULL, fb_ioctl_op))
        panic("init_fb_dev: failed to register /dev/fb0");
    printk("devfs: /dev/fb0 registered\n");
}
