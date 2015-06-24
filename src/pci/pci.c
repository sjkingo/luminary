#include <stdint.h>

#include "pci/pci.h"
#include "pci/io.h"
#include "kernel.h"

static void scan_device(uint32_t bus, uint32_t dev, uint32_t func)
{
    uint32_t id = PCI_MAKE_ID(bus, dev, func);
    struct pci_device device;

    device.vendor_id = pci_inb_16(id, PCI_CONFIG_VENDOR_ID);
    if (device.vendor_id == 0xffff)
        return;
    device.device_id = pci_inb_16(id, PCI_CONFIG_DEVICE_ID);
    device.class_code = pci_inb_8(id, PCI_CONFIG_CLASS_CODE);
    device.subclass = pci_inb_8(id, PCI_CONFIG_SUBCLASS);
    device.prog_intf = pci_inb_8(id, PCI_CONFIG_PROG_INTF);
    device.type_id = ((device.class_code << 8) | device.subclass);

#ifdef DEBUG
    printk("  %02x:%02x:%d: 0x%04x/0x%04x (0x%04x)\n", bus, dev, func, 
            device.vendor_id, device.device_id, device.type_id);
#endif
}

static void scan_pci_bus(void)
{
#ifdef DEBUG
    printk("Scanning PCI bus for devices..\n");
#endif
    for (uint32_t bus = 0; bus < 256; bus++)
        for (uint32_t dev = 0; dev < 32; dev++)
            for (uint32_t func = 0; func < 8; func++)
                scan_device(bus, dev, func);               
}

void init_pci(void)
{
    scan_pci_bus();
}
