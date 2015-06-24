#include <stdint.h>
#include <stdlib.h>

#include "pci/register.h"
#include "pci/pci.h"

/* in drivers/rtl8139.c */
extern void rtl8139_init(struct pci_device_location *);

/* The array of known devices. See http://www.pcidatabase.com/ */
struct device_driver drivers[] = {
    /* Intel system devices */
    { 0x8086, 0x1237, "Intel i440FX Chipset", 0 },
    { 0x8086, 0x7000, "Intel PIIX3 PCI-to-ISA Bridge (Triton II)", 0 },
    { 0x8086, 0x7010, "Intel PIIX3 IDE Interface (Triton II)", 0 },
    { 0x8086, 0x7113, "Intel PIIX4/4E/4M Power Management Controller", 0 },

    /* Network devices */
    { 0x10ec, 0x8139, "Realtek RTL8139 10/100 NIC", rtl8139_init },
    { 0x8086, 0x100e, "Intel Pro 1000/MT NIC", 0 },

    /* VGA devices */
    { 0x1234, 0x1111, "QEMU/Bochs VBE Framebuffer", 0 },
};

struct device_driver *lookup_driver(uint16_t vendor_id, uint16_t device_id)
{
    uint32_t n = sizeof(drivers) / sizeof(struct device_driver);
    for (uint32_t i = 0; i < n; i++) {
        if (drivers[i].vendor_id == vendor_id && drivers[i].device_id == device_id) {
            return &drivers[i];
        }
    }
    return NULL;
}
