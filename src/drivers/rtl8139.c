#define MODULE "rtl8139: "

#include "kernel/kernel.h"
#include "kernel/printk.h"
#include "pci/pci.h"
#include "pci/io.h"

/* RTL8139 simple PCI network chip
 * http://wiki.osdev.org/RTL8139
 */

/* The controller is mapped in memory and is accessed as an offset
 * of the I/O base address. These constants define the offsets for
 * each port on the chip.
 */
#define RTL_PORT_MAC	        0x00
#define RTL_PORT_RBSTART        0x30
#define RTL_PORT_IMR            0x3C
#define RTL_PORT_CMD            0x37
#define RTL_PORT_RXMISS         0x4C
#define RTL_PORT_TCR            0x40
#define RTL_PORT_RCR            0x44
#define RTL_PORT_CONFIG         0x52

#define RTL_RX_BUF_SIZE         (8192+16)

static int irq = 0;
static uint32_t iobase = 0;
static uint8_t mac[6];
static uint8_t buffer_rx[RTL_RX_BUF_SIZE];

void rtl8139_init(struct pci_device_location *loc)
{
    uint32_t pci_id = pci_device(loc->bus, loc->dev, loc->func);

    /* Attempt to enable bus mastering */
    uint16_t r = pci_read_field(pci_id, PCI_CONFIG_COMMAND, 4);
    r |= (1 << 2); /* set bit 2 */
    pci_outb(pci_id, PCI_CONFIG_COMMAND, r);

    /* Confirm bus mastering is now enabled */
    r = pci_read_field(pci_id, PCI_CONFIG_COMMAND, 4);
    if (r & (1 << 2)) {
        printk(MODULE "bus mastering is enabled\n");
    } else {
        panic(MODULE "could not enable bus mastering");
    }

    /* Set up interrupts */
    irq = pci_read_field(pci_id, PCI_INTERRUPT_LINE, 1);
    printk(MODULE "interrupt line at %x\n", irq);
    // TODO

    /* Read I/O base address */
    uint32_t bar0 = pci_read_field(pci_id, PCI_BAR0, 4);
    uint32_t bar1 = pci_read_field(pci_id, PCI_BAR1, 4);
    if (bar0 & 0x00000001) {
	iobase = bar0 & 0xFFFFFFFC;
    } else {
	panic(MODULE "could not get I/O base address after bus mastering enabled");
    }
    printk(MODULE "iobase: 0x%x\n", iobase);

    /* Read MAC address */
    for (int i = 0; i < 6; ++i) {
        mac[i] = inb(iobase + RTL_PORT_MAC + i);
    }
    printk(MODULE "mac address %02x:%02x:%02x:%02x:%02x:%02x\n", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    /* Enable and reset the chip */
    outb(iobase + RTL_PORT_CONFIG, 0x0);
    outb(iobase + RTL_PORT_CMD, 0x10);
    while ((inb(iobase + RTL_PORT_CMD) & 0x10) != 0);
    printk(MODULE "device is ready\n");

    /* Enable interrupts for the chip */
    outb_16(iobase + RTL_PORT_IMR, 0x0005); // Tx OK and Rx OK

    /* Configure tx and rx buffers */
    outb_32(iobase + RTL_PORT_RBSTART, (uintptr_t)buffer_rx);
    outb_32(iobase + RTL_PORT_TCR, 0);
    outb_32(iobase + RTL_PORT_RCR,
            0xf |       // broadcast, multicast, physical and promiscuous
            (0 << 7)    // wrap as ring buffer (bit 0)
    );
    outb(iobase + RTL_PORT_CMD, 0x0C); // enable tx and rx
}
