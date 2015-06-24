#pragma once

#include <stdint.h>

#include "pci/pci.h"

struct device_driver {
    uint16_t vendor_id;
    uint16_t device_id;
    char *name;
    void (*init_func)(struct pci_device_location *);
};

extern struct device_driver drivers[];

struct device_driver *lookup_driver(uint16_t vendor_id, uint16_t device_id);
