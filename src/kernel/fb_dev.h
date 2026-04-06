#pragma once

#include <stdint.h>

/* ioctl request code for /dev/fb0 */
#define FBIOGET_INFO  1

/* Filled by ioctl(fd, FBIOGET_INFO, struct fb_info *) */
struct fb_info {
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint8_t  depth;   /* bits per pixel */
    uint32_t fb_addr; /* physical address (identity-mapped, userland-accessible) */
};

void init_fb_dev(void);
