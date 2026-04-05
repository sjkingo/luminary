/* Block device abstraction layer.
 *
 * Maintains a registry of up to BLKDEV_MAX block devices. Each registered
 * device gets a /dev/<name> chardev node; reads and writes are translated
 * from byte offsets to sector-aligned LBA requests.
 *
 * The VFS chardev op signature (offset, len, buf) carries no context pointer,
 * so each slot gets its own generated op functions via BLKDEV_SLOT(i), the
 * same technique used by pipe.c.
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "drivers/blkdev.h"
#include "kernel/vfs.h"
#include "kernel/kernel.h"

#define MODULE "blkdev: "

/* Inode base for /dev/hda etc. — above devfs (99-102) and pipe (200+) nodes */
#define BLKDEV_INODE_BASE 300

static struct blkdev *blkdev_table[BLKDEV_MAX];
static uint32_t blkdev_count = 0;

int blkdev_register(struct blkdev *dev)
{
    if (blkdev_count >= BLKDEV_MAX) {
        printk(MODULE "device table full, cannot register %s\n", dev->name);
        return -1;
    }
    blkdev_table[blkdev_count++] = dev;
    return 0;
}

struct blkdev *blkdev_find(const char *name)
{
    for (uint32_t i = 0; i < blkdev_count; i++) {
        if (strcmp(blkdev_table[i]->name, name) == 0)
            return blkdev_table[i];
    }
    return NULL;
}

struct blkdev *blkdev_get(uint32_t index)
{
    if (index >= blkdev_count) return NULL;
    return blkdev_table[index];
}

/* Core read/write/control logic — called by per-slot op functions */

static uint32_t blkdev_do_read(uint32_t slot, uint32_t offset,
                               uint32_t len, void *buf)
{
    struct blkdev *dev = blkdev_table[slot];
    if (!dev || !dev->read_sectors || len == 0) return 0;

    uint8_t *dst = (uint8_t *)buf;
    uint32_t copied = 0;
    uint8_t  tmp[BLKDEV_SECTOR_SIZE];

    while (copied < len) {
        uint32_t lba        = (offset + copied) / BLKDEV_SECTOR_SIZE;
        uint32_t sec_off    = (offset + copied) % BLKDEV_SECTOR_SIZE;
        uint32_t avail      = BLKDEV_SECTOR_SIZE - sec_off;
        uint32_t chunk      = len - copied;
        if (chunk > avail) chunk = avail;

        if (lba >= dev->sector_count) break;

        if (sec_off == 0 && chunk == BLKDEV_SECTOR_SIZE) {
            if (dev->read_sectors(dev, lba, 1, dst + copied) != 1) break;
        } else {
            if (dev->read_sectors(dev, lba, 1, tmp) != 1) break;
            memcpy(dst + copied, tmp + sec_off, chunk);
        }
        copied += chunk;
    }
    return copied;
}

static uint32_t blkdev_do_write(uint32_t slot, uint32_t offset,
                                uint32_t len, const void *buf)
{
    struct blkdev *dev = blkdev_table[slot];
    if (!dev || !dev->write_sectors || len == 0) return 0;

    const uint8_t *src = (const uint8_t *)buf;
    uint32_t written = 0;
    uint8_t  tmp[BLKDEV_SECTOR_SIZE];

    while (written < len) {
        uint32_t lba        = (offset + written) / BLKDEV_SECTOR_SIZE;
        uint32_t sec_off    = (offset + written) % BLKDEV_SECTOR_SIZE;
        uint32_t avail      = BLKDEV_SECTOR_SIZE - sec_off;
        uint32_t chunk      = len - written;
        if (chunk > avail) chunk = avail;

        if (lba >= dev->sector_count) break;

        if (sec_off == 0 && chunk == BLKDEV_SECTOR_SIZE) {
            if (dev->write_sectors(dev, lba, 1, src + written) != 1) break;
        } else {
            /* read-modify-write for partial sectors */
            if (dev->read_sectors(dev, lba, 1, tmp) != 1) break;
            memcpy(tmp + sec_off, src + written, chunk);
            if (dev->write_sectors(dev, lba, 1, tmp) != 1) break;
        }
        written += chunk;
    }
    return written;
}

static int32_t blkdev_do_control(uint32_t slot, struct vfs_node *node,
                                 uint32_t request, void *arg)
{
    (void)node;
    struct blkdev *dev = blkdev_table[slot];
    if (!dev) return -1;

    if (request == BLKDEV_IOCTL_GETSIZE) {
        uint64_t *out = (uint64_t *)arg;
        *out = (uint64_t)dev->sector_count * BLKDEV_SECTOR_SIZE;
        return 0;
    }
    if (request == BLKDEV_IOCTL_GETSECTSZ) {
        uint32_t *out = (uint32_t *)arg;
        *out = BLKDEV_SECTOR_SIZE;
        return 0;
    }
    return -1;
}

/* Per-slot op functions — each closes over its slot index */

#define BLKDEV_SLOT(i) \
static uint32_t blkdev_read_op_##i(uint32_t off, uint32_t len, void *buf) \
    { return blkdev_do_read(i, off, len, buf); } \
static uint32_t blkdev_write_op_##i(uint32_t off, uint32_t len, const void *buf) \
    { return blkdev_do_write(i, off, len, buf); } \
static int32_t blkdev_ctrl_op_##i(struct vfs_node *n, uint32_t req, void *arg) \
    { return blkdev_do_control(i, n, req, arg); }

BLKDEV_SLOT(0) BLKDEV_SLOT(1) BLKDEV_SLOT(2) BLKDEV_SLOT(3)
BLKDEV_SLOT(4) BLKDEV_SLOT(5) BLKDEV_SLOT(6) BLKDEV_SLOT(7)

typedef uint32_t (*blkdev_read_op_t)(uint32_t, uint32_t, void *);
typedef uint32_t (*blkdev_write_op_t)(uint32_t, uint32_t, const void *);
typedef int32_t  (*blkdev_ctrl_op_t)(struct vfs_node *, uint32_t, void *);

static const blkdev_read_op_t  slot_read[BLKDEV_MAX] = {
    blkdev_read_op_0, blkdev_read_op_1, blkdev_read_op_2, blkdev_read_op_3,
    blkdev_read_op_4, blkdev_read_op_5, blkdev_read_op_6, blkdev_read_op_7,
};
static const blkdev_write_op_t slot_write[BLKDEV_MAX] = {
    blkdev_write_op_0, blkdev_write_op_1, blkdev_write_op_2, blkdev_write_op_3,
    blkdev_write_op_4, blkdev_write_op_5, blkdev_write_op_6, blkdev_write_op_7,
};
static const blkdev_ctrl_op_t  slot_ctrl[BLKDEV_MAX] = {
    blkdev_ctrl_op_0, blkdev_ctrl_op_1, blkdev_ctrl_op_2, blkdev_ctrl_op_3,
    blkdev_ctrl_op_4, blkdev_ctrl_op_5, blkdev_ctrl_op_6, blkdev_ctrl_op_7,
};

void blkdev_register_devnode(struct blkdev *dev)
{
    uint32_t slot = blkdev_count - 1;

    struct vfs_node *n = vfs_register_dev(
        dev->name,
        BLKDEV_INODE_BASE + slot,
        slot_read[slot],
        slot_write[slot],
        slot_ctrl[slot]);

    if (!n) {
        printk(MODULE "failed to create /dev/%s\n", dev->name);
        return;
    }

    n->size = dev->sector_count * BLKDEV_SECTOR_SIZE;
    DBGK("/dev/%s registered: %ld sectors, node size %ld bytes\n",
         dev->name, dev->sector_count, n->size);
}
