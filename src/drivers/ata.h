#pragma once

#include <stdint.h>

/* ATA channel base I/O ports */
#define ATA_PRIMARY_BASE    0x1F0
#define ATA_PRIMARY_CTRL    0x3F6
#define ATA_SECONDARY_BASE  0x170
#define ATA_SECONDARY_CTRL  0x376

/* Register offsets from I/O base */
#define ATA_REG_DATA        0x00    /* R/W 16-bit data port */
#define ATA_REG_ERROR       0x01    /* R: error */
#define ATA_REG_FEATURES    0x01    /* W: features */
#define ATA_REG_SECCOUNT    0x02    /* sector count (LBA28 low) */
#define ATA_REG_LBA0        0x03    /* LBA bits 0-7 */
#define ATA_REG_LBA1        0x04    /* LBA bits 8-15 */
#define ATA_REG_LBA2        0x05    /* LBA bits 16-23 */
#define ATA_REG_HDDEVSEL    0x06    /* drive/head select */
#define ATA_REG_STATUS      0x07    /* R: status */
#define ATA_REG_COMMAND     0x07    /* W: command */

/* Status register bits */
#define ATA_SR_BSY          0x80    /* busy */
#define ATA_SR_DRDY         0x40    /* drive ready */
#define ATA_SR_DRQ          0x08    /* data request */
#define ATA_SR_ERR          0x01    /* error */

/* ATA commands */
#define ATA_CMD_READ_PIO    0x20
#define ATA_CMD_WRITE_PIO   0x30
#define ATA_CMD_FLUSH_CACHE 0xE7
#define ATA_CMD_IDENTIFY    0xEC

/* IDENTIFY data word indices */
#define ATA_IDENT_CAPS      49      /* bit 9: LBA supported */
#define ATA_IDENT_LBA28_LO  60      /* LBA28 sector count low word */
#define ATA_IDENT_LBA28_HI  61      /* LBA28 sector count high word */
#define ATA_IDENT_MODEL     27      /* model string: 40 bytes (words 27-46) */

struct ata_drive {
    uint16_t io_base;
    uint16_t ctrl_base;
    uint8_t  slave;         /* 0 = master, 1 = slave */
    uint8_t  present;
    uint32_t sectors;       /* LBA28 sector count */
    char     model[41];
};

void init_ata(void);
