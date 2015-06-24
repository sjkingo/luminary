#pragma once

#include <stdint.h>

struct pci_device {
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t prog_intf;
};

struct pci_device_location {
    uint32_t bus;
    uint32_t dev;
    uint32_t func;
};

void init_pci(void);
