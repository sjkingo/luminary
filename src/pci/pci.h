#pragma once

#include <stdint.h>

struct pci_device {
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t prog_intf;
    uint8_t type_id;
};

void init_pci(void);
