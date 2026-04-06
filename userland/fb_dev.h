/* fb_dev.h — userland /dev/fb0 ioctl API. */
#pragma once

#include "syscall.h"

#define FBIOGET_INFO  1

struct fb_info {
    unsigned int width;
    unsigned int height;
    unsigned int pitch;
    unsigned char depth;   /* bits per pixel */
    unsigned int fb_addr;  /* physical address (identity-mapped, directly writable) */
};

static inline int fb_get_info(int fd, struct fb_info *info)
{
    return ioctl(fd, FBIOGET_INFO, info);
}
