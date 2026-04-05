#pragma once

#include <stdint.h>

#define BLKDEV_SECTOR_SIZE  512
#define BLKDEV_MAX          8

struct vfs_node;

struct blkdev {
    char     name[16];
    uint32_t sector_count;
    uint8_t  lba48;
    uint64_t sector_count_48;

    uint32_t (*read_sectors)(struct blkdev *dev, uint32_t lba,
                             uint32_t count, void *buf);
    uint32_t (*write_sectors)(struct blkdev *dev, uint32_t lba,
                              uint32_t count, const void *buf);

    void *private;
};

/* ioctl request codes for block device nodes */
#define BLKDEV_IOCTL_GETSIZE   1    /* arg: uint64_t * — total size in bytes */
#define BLKDEV_IOCTL_GETSECTSZ 2    /* arg: uint32_t * — sector size (always 512) */

int            blkdev_register(struct blkdev *dev);
struct blkdev *blkdev_find(const char *name);
struct blkdev *blkdev_get(uint32_t index);
void           blkdev_register_devnode(struct blkdev *dev);
