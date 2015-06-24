#ifdef USE_SERIAL

#include "cpu/x86.h"
#include "drivers/serial.h"
#include "kernel/printk.h"

static int is_transmit_empty(void)
{
    return inb(SERIAL_COM1_PORT + 5) & 0x20;
}
 
void write_serial(char a)
{
    /* Wait for the queue to clear */
    while (is_transmit_empty() == 0);
    outb(SERIAL_COM1_PORT, a);
}

void serial_init(void)
{
    outb(SERIAL_COM1_PORT + 1, 0x00); // Disable all interrupts
    outb(SERIAL_COM1_PORT + 3, 0x80); // Enable DLAB (set baud rate divisor)
    outb(SERIAL_COM1_PORT + 0, 0x03); // Set divisor to 3 (lo byte) 38400 baud
    outb(SERIAL_COM1_PORT + 1, 0x00); //                  (hi byte)
    outb(SERIAL_COM1_PORT + 3, 0x03); // 8 bits, no parity, one stop bit
    outb(SERIAL_COM1_PORT + 2, 0xC7); // Enable FIFO, clear them, with 14-byte threshold
    outb(SERIAL_COM1_PORT + 4, 0x0B); // IRQs enabled, RTS/DSR set
#ifdef DEBUG
    printk("Built with serial support: console output will go to COM1\n");
#endif
}

#endif
