#pragma once

#include "cpu/x86.h"

/* PCI I/O ports */
#define PCI_CONFIG_ADDR                 0xcf8
#define PCI_CONFIG_DATA                 0xcfC

/* PCI config registers */
#define PCI_CONFIG_VENDOR_ID            0x00
#define PCI_CONFIG_DEVICE_ID            0x02
#define PCI_CONFIG_PROG_INTF            0x09
#define PCI_CONFIG_SUBCLASS             0x0a
#define PCI_CONFIG_CLASS_CODE           0x0b
#define PCI_CONFIG_HEADER_TYPE          0x0e

/* Compute the ID of a PCI device */
#define PCI_MAKE_ID(bus, dev, func)     ((bus << 16) | (dev << 11) | (func << 8))

static inline uint16_t pci_inb_16(uint32_t id, uint32_t reg)
{
    uint32_t addr = 0x80000000 | id | (reg & 0xfc);
    outb_32(PCI_CONFIG_ADDR, addr);
    return inb_16(PCI_CONFIG_DATA + (reg & 0x02));
}

static inline uint8_t pci_inb_8(uint32_t id, uint32_t reg)
{
    uint32_t addr = 0x80000000 | id | (reg & 0xfc);
    outb_32(PCI_CONFIG_ADDR, addr);
    return inb(PCI_CONFIG_DATA + (reg & 0x03));
}
