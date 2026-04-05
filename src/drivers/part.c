/* MBR partition table probe.
 *
 * Reads sector 0 of a block device, validates the MBR signature (0x55AA at
 * bytes 510-511), and registers up to 4 primary partitions as child blkdevs.
 * Partition names are derived from the parent: "hda" -> "hda1".."hda4".
 *
 * Each partition blkdev offsets LBA addresses by the partition's start sector.
 * The per-slot BLKDEV_SLOT macro technique is not needed here — the partition
 * blkdev ops call into blkdev_do_read/write via the parent blkdev pointer
 * stored in the private field.
 */

#include <stdint.h>
#include <string.h>

#include "drivers/part.h"
#include "drivers/blkdev.h"
#include "kernel/kernel.h"
#include "kernel/heap.h"

#define MODULE "part: "

/* MBR partition entry at offset 446 + n*16 */
struct mbr_entry {
    uint8_t  status;
    uint8_t  chs_first[3];
    uint8_t  type;
    uint8_t  chs_last[3];
    uint32_t lba_start;
    uint32_t lba_count;
} __attribute__((packed));

/* Up to 4 drives × 4 partitions */
#define PART_MAX 16

struct part_priv {
    struct blkdev *parent;
    uint32_t       lba_offset;
};

static struct part_priv  part_privs[PART_MAX];
static struct blkdev     part_devs[PART_MAX];
static uint32_t          part_count = 0;

static uint32_t part_read(struct blkdev *dev, uint32_t lba,
                           uint32_t count, void *buf)
{
    struct part_priv *priv = (struct part_priv *)dev->private;
    return priv->parent->read_sectors(priv->parent,
                                      priv->lba_offset + lba, count, buf);
}

static uint32_t part_write(struct blkdev *dev, uint32_t lba,
                            uint32_t count, const void *buf)
{
    struct part_priv *priv = (struct part_priv *)dev->private;
    return priv->parent->write_sectors(priv->parent,
                                       priv->lba_offset + lba, count, buf);
}

void part_probe(struct blkdev *disk)
{
    uint8_t sector[BLKDEV_SECTOR_SIZE];

    if (disk->read_sectors(disk, 0, 1, sector) != 1) {
        DBGK("%s: failed to read MBR\n", disk->name);
        return;
    }

    if (sector[510] != 0x55 || sector[511] != 0xAA) {
        DBGK("%s: no MBR signature (got 0x%lx 0x%lx)\n",
             disk->name, (uint32_t)sector[510], (uint32_t)sector[511]);
        return;
    }

    DBGK("%s: valid MBR\n", disk->name);

    struct mbr_entry *table = (struct mbr_entry *)(sector + 446);

    for (int i = 0; i < 4; i++) {
        struct mbr_entry *e = &table[i];

        if (e->type == 0 || e->lba_count == 0)
            continue;

        if (part_count >= PART_MAX) {
            printk(MODULE "partition table full\n");
            return;
        }

        uint32_t slot = part_count++;

        struct part_priv *priv = &part_privs[slot];
        priv->parent     = disk;
        priv->lba_offset = e->lba_start;

        struct blkdev *bd = &part_devs[slot];
        memset(bd, 0, sizeof(*bd));

        /* Build name: parent name + partition number (1-based) */
        uint32_t nlen = (uint32_t)strlen(disk->name);
        if (nlen >= sizeof(bd->name) - 2) nlen = sizeof(bd->name) - 2;
        memcpy(bd->name, disk->name, nlen);
        bd->name[nlen]     = '0' + (i + 1);
        bd->name[nlen + 1] = '\0';

        bd->sector_count  = e->lba_count;
        bd->read_sectors  = part_read;
        bd->write_sectors = part_write;
        bd->private       = priv;

        printk(MODULE "%s: type 0x%lx, lba %ld+%ld (%ld MB)\n",
               bd->name, (uint32_t)e->type,
               e->lba_start, e->lba_count,
               e->lba_count / 2048);

        if (blkdev_register(bd) < 0) return;
        blkdev_register_devnode(bd);
    }
}
