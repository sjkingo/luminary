/* ATA PIO driver.
 *
 * Probes primary and secondary IDE channels for up to four drives using the
 * IDENTIFY command. Present drives are registered as block devices (hda–hdd)
 * with /dev nodes created via blkdev_register_devnode().
 *
 * Uses pure polling (no interrupts). Drive interrupts are masked via the
 * nIEN bit in the device control register at init time.
 *
 * Only LBA28 is implemented. Maximum addressable capacity per drive: 128GB.
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "drivers/ata.h"
#include "drivers/blkdev.h"
#include "drivers/part.h"
#include "kernel/kernel.h"
#include "cpu/x86.h"

#define MODULE "ata: "

/* Up to 4 drives: primary master/slave, secondary master/slave */
static struct ata_drive drives[4];
static struct blkdev     blkdevs[4];

/* Read the alternate status register four times to give the drive ~400ns */
static void ata_delay(uint16_t ctrl_base)
{
    inb(ctrl_base);
    inb(ctrl_base);
    inb(ctrl_base);
    inb(ctrl_base);
}

/* Poll until BSY clears. Returns the final status byte. */
static uint8_t ata_poll(uint16_t io_base, uint16_t ctrl_base)
{
    ata_delay(ctrl_base);
    uint8_t st;
    do {
        st = inb(io_base + ATA_REG_STATUS);
    } while (st & ATA_SR_BSY);
    return st;
}

/* Maximum sectors per ATA PIO command (LBA28: 0 = 256, but we cap at 255
 * to keep the count in a uint8_t without ambiguity). */
#define ATA_MAX_SECTORS_PER_CMD 255

static uint32_t ata_read_sectors(struct blkdev *dev, uint32_t lba,
                                 uint32_t count, void *buf)
{
    struct ata_drive *drv = (struct ata_drive *)dev->private;
    uint16_t base  = drv->io_base;
    uint16_t ctrl  = drv->ctrl_base;
    uint8_t  slave = drv->slave;
    uint8_t *dst   = (uint8_t *)buf;
    uint32_t done  = 0;

    while (done < count) {
        uint32_t batch = count - done;
        if (batch > ATA_MAX_SECTORS_PER_CMD)
            batch = ATA_MAX_SECTORS_PER_CMD;
        uint32_t cur_lba = lba + done;

        uint8_t st = ata_poll(base, ctrl);
        if (st & ATA_SR_ERR) {
            DBGK("read error before lba %ld, status 0x%lx\n",
                 cur_lba, (uint32_t)st);
            return done;
        }

        outb(base + ATA_REG_HDDEVSEL,
             0xE0 | (slave << 4) | ((cur_lba >> 24) & 0x0F));
        outb(base + ATA_REG_SECCOUNT, (uint8_t)batch);
        outb(base + ATA_REG_LBA0,  (uint8_t)(cur_lba));
        outb(base + ATA_REG_LBA1,  (uint8_t)(cur_lba >> 8));
        outb(base + ATA_REG_LBA2,  (uint8_t)(cur_lba >> 16));
        outb(base + ATA_REG_COMMAND, ATA_CMD_READ_PIO);

        for (uint32_t i = 0; i < batch; i++) {
            st = ata_poll(base, ctrl);
            if (st & ATA_SR_ERR) {
                DBGK("read error on sector %ld (lba %ld), status 0x%lx, err 0x%lx\n",
                     done + i, cur_lba + i, (uint32_t)st,
                     (uint32_t)inb(base + ATA_REG_ERROR));
                return done + i;
            }
            if (!(st & ATA_SR_DRQ)) {
                DBGK("DRQ not set after read command, lba %ld, status 0x%lx\n",
                     cur_lba + i, (uint32_t)st);
                return done + i;
            }
            uint16_t *words = (uint16_t *)(dst + (done + i) * BLKDEV_SECTOR_SIZE);
            for (int w = 0; w < 256; w++)
                words[w] = inb_16(base + ATA_REG_DATA);
        }

        done += batch;
    }

    return count;
}

static uint32_t ata_write_sectors(struct blkdev *dev, uint32_t lba,
                                  uint32_t count, const void *buf)
{
    struct ata_drive *drv  = (struct ata_drive *)dev->private;
    uint16_t base   = drv->io_base;
    uint16_t ctrl   = drv->ctrl_base;
    uint8_t  slave  = drv->slave;
    const uint8_t *src = (const uint8_t *)buf;

    for (uint32_t i = 0; i < count; i++) {
        uint32_t cur_lba = lba + i;

        uint8_t st = ata_poll(base, ctrl);
        if (st & ATA_SR_ERR) {
            DBGK("write error before sector %ld (lba %ld), status 0x%lx\n",
                 (uint32_t)i, cur_lba, (uint32_t)st);
            return i;
        }

        outb(base + ATA_REG_HDDEVSEL,
             0xE0 | (slave << 4) | ((cur_lba >> 24) & 0x0F));
        outb(base + ATA_REG_SECCOUNT, 1);
        outb(base + ATA_REG_LBA0,  (uint8_t)(cur_lba));
        outb(base + ATA_REG_LBA1,  (uint8_t)(cur_lba >> 8));
        outb(base + ATA_REG_LBA2,  (uint8_t)(cur_lba >> 16));
        outb(base + ATA_REG_COMMAND, ATA_CMD_WRITE_PIO);

        st = ata_poll(base, ctrl);
        if (!(st & ATA_SR_DRQ)) {
            DBGK("DRQ not set after write command, lba %ld, status 0x%lx\n",
                 cur_lba, (uint32_t)st);
            return i;
        }

        const uint16_t *words = (const uint16_t *)(src + i * BLKDEV_SECTOR_SIZE);
        for (int w = 0; w < 256; w++)
            outb_16(base + ATA_REG_DATA, words[w]);

        /* Flush write cache */
        outb(base + ATA_REG_COMMAND, ATA_CMD_FLUSH_CACHE);
        ata_poll(base, ctrl);
    }

    return count;
}

static void ata_probe(struct ata_drive *drv, uint16_t io_base,
                      uint16_t ctrl_base, uint8_t slave,
                      const char *name, uint32_t drive_idx)
{
    drv->io_base   = io_base;
    drv->ctrl_base = ctrl_base;
    drv->slave     = slave;
    drv->present   = 0;

    /* Disable drive interrupts (nIEN bit) */
    outb(ctrl_base, 0x02);

    /* Select drive */
    outb(io_base + ATA_REG_HDDEVSEL, 0xA0 | (slave << 4));
    ata_delay(ctrl_base);

    /* Check if anything is there */
    uint8_t st = inb(io_base + ATA_REG_STATUS);
    if (st == 0xFF) {
        DBGK("%s: no device (status 0xFF)\n", name);
        return;
    }

    /* Zero LBA and sector count registers before IDENTIFY */
    outb(io_base + ATA_REG_SECCOUNT, 0);
    outb(io_base + ATA_REG_LBA0, 0);
    outb(io_base + ATA_REG_LBA1, 0);
    outb(io_base + ATA_REG_LBA2, 0);
    outb(io_base + ATA_REG_COMMAND, ATA_CMD_IDENTIFY);
    ata_delay(ctrl_base);

    st = inb(io_base + ATA_REG_STATUS);
    if (st == 0x00) {
        DBGK("%s: no device after IDENTIFY (status 0x00)\n", name);
        return;
    }

    /* If LBA1/LBA2 are non-zero this is ATAPI, skip */
    uint8_t lba1 = inb(io_base + ATA_REG_LBA1);
    uint8_t lba2 = inb(io_base + ATA_REG_LBA2);
    if (lba1 || lba2) {
        DBGK("%s: ATAPI device, skipping (lba1=0x%lx lba2=0x%lx)\n",
             name, (uint32_t)lba1, (uint32_t)lba2);
        return;
    }

    /* Poll until BSY clears and DRQ or ERR is set */
    do {
        st = inb(io_base + ATA_REG_STATUS);
    } while ((st & ATA_SR_BSY) && !(st & ATA_SR_ERR));

    if (st & ATA_SR_ERR) {
        DBGK("%s: IDENTIFY error, status 0x%lx\n", name, (uint32_t)st);
        return;
    }

    /* Read 256 words of IDENTIFY data */
    uint16_t ident[256];
    for (int i = 0; i < 256; i++)
        ident[i] = inb_16(io_base + ATA_REG_DATA);

    drv->sectors = ((uint32_t)ident[ATA_IDENT_LBA28_HI] << 16) |
                    (uint32_t)ident[ATA_IDENT_LBA28_LO];

    /* Extract model string — IDENTIFY words are byte-swapped */
    for (int i = 0; i < 20; i++) {
        uint16_t w = ident[ATA_IDENT_MODEL + i];
        drv->model[i * 2]     = (char)(w >> 8);
        drv->model[i * 2 + 1] = (char)(w & 0xFF);
    }
    drv->model[40] = '\0';

    /* Strip trailing spaces */
    for (int i = 39; i >= 0 && drv->model[i] == ' '; i--)
        drv->model[i] = '\0';

    drv->present = 1;

    printk(MODULE "%s: %s (%ld sectors, %ld MB)\n",
           name, drv->model, drv->sectors,
           drv->sectors / 2048);
    DBGK("%s: io_base 0x%lx ctrl 0x%lx slave %ld\n",
         name, (uint32_t)io_base, (uint32_t)ctrl_base, (uint32_t)slave);

    /* Register as block device */
    struct blkdev *bd = &blkdevs[drive_idx];
    memset(bd, 0, sizeof(*bd));
    strncpy(bd->name, name, sizeof(bd->name) - 1);
    bd->sector_count   = drv->sectors;
    bd->read_sectors   = ata_read_sectors;
    bd->write_sectors  = ata_write_sectors;
    bd->private        = drv;

    if (blkdev_register(bd) < 0) return;
    blkdev_register_devnode(bd);
    part_probe(bd);
}

void init_ata(void)
{
    DBGK("probing ATA channels\n");

    ata_probe(&drives[0], ATA_PRIMARY_BASE,   ATA_PRIMARY_CTRL,   0, "hda", 0);
    ata_probe(&drives[1], ATA_PRIMARY_BASE,   ATA_PRIMARY_CTRL,   1, "hdb", 1);
    ata_probe(&drives[2], ATA_SECONDARY_BASE, ATA_SECONDARY_CTRL, 0, "hdc", 2);
    ata_probe(&drives[3], ATA_SECONDARY_BASE, ATA_SECONDARY_CTRL, 1, "hdd", 3);
}
