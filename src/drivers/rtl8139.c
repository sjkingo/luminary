#include "kernel/printk.h"
#include "pci/pci.h"

void rtl8139_init(struct pci_device_location *loc)
{
#ifdef DEBUG
    printk("rtl8139 chip init at %02d:%02d:%d\n", loc->bus, loc->dev, loc->func);
    printk("TODO\n");
#endif
}
