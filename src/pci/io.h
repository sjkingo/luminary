#pragma once

#include "cpu/x86.h"

/* PCI I/O ports */
#define PCI_CONFIG_ADDR                 0xcf8
#define PCI_CONFIG_DATA                 0xcfC

/* PCI config registers */
#define PCI_CONFIG_VENDOR_ID            0x00
#define PCI_CONFIG_DEVICE_ID            0x02
#define PCI_CONFIG_COMMAND              0x04
#define PCI_CONFIG_PROG_INTF            0x09
#define PCI_CONFIG_SUBCLASS             0x0a
#define PCI_CONFIG_CLASS_CODE           0x0b
#define PCI_CONFIG_HEADER_TYPE          0x0e

#define PCI_INTERRUPT_LINE              0x3c

#define PCI_BAR0			0x10
#define PCI_BAR1			0x14

static inline int pci_extract_bus(uint32_t device) {
    return (uint8_t)((device >> 16));
}
static inline int pci_extract_slot(uint32_t device) {
    return (uint8_t)((device >> 8));
}
static inline int pci_extract_func(uint32_t device) {
    return (uint8_t)(device);
}

static inline uint32_t pci_get_addr(uint32_t device, int field) {
	return 0x80000000 | (pci_extract_bus(device) << 16) | (pci_extract_slot(device) << 11) | (pci_extract_func(device) << 8) | ((field) & 0xFC);
}

static inline uint32_t pci_device(int bus, int dev, int func) {
	return (uint32_t)((bus << 16) | (dev << 8) | func);
}

static uint32_t pci_read_field(uint32_t device, int field, int size) {
    outb_32(PCI_CONFIG_ADDR, pci_get_addr(device, field));

    if (size == 4) {
	uint32_t t = inb_32(PCI_CONFIG_DATA);
	return t;
    } else if (size == 2) {
	uint16_t t = inb_16(PCI_CONFIG_DATA + (field & 2));
	return t;
    } else if (size == 1) {
	uint8_t t = inb(PCI_CONFIG_DATA + (field & 3));
	return t;
    }
    return 0xFFFF;
}

static inline void pci_outb(uint32_t id, int field, uint32_t value) {
    outb(PCI_CONFIG_ADDR, pci_get_addr(id, field));
    outb(PCI_CONFIG_DATA, value);
}
