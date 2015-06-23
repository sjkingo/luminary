#include "kernel.h"
#include "pic.h"
#include "traps.h"
#include "x86.h"

static void timer_init(void)
{
    unsigned int div = CLOCK / 50; // 50ms
    outb(0x43, 0x36); /* command byte */
    outb(0x40, (unsigned char)(div & 0xFF));
    outb(0x40, (unsigned char)((div >> 8) & 0xFF));
}

static inline void ack_irq(int irq)
{
    if (irq >= IRQ_SLAVE_OFFSET) {
        printk("ACK'ing slave as irq=%d and IRQ_SLAVE_OFFSET=%d\n", irq, IRQ_SLAVE_OFFSET);
        outb(PIC_SLAVE_CMD, PIC_EOI);
    }
    outb(PIC_MASTER_CMD, PIC_EOI);
}

void pic_init(void)
{
    /* Re-map the IRQ table to CPU vectors [32..48] */
    outb(PIC_MASTER_CMD, PIC_INIT | PIC_ICW4);
    outb(PIC_SLAVE_CMD, PIC_INIT | PIC_ICW4);
    outb(PIC_MASTER_DATA, IRQ_BASE_OFFSET);
    outb(PIC_SLAVE_DATA, IRQ_SLAVE_OFFSET);
    outb(PIC_MASTER_DATA, 4); /* slave at IRQ2 */
    outb(PIC_SLAVE_DATA, 2); /* cascade */
    outb(PIC_MASTER_DATA, PIC_8086);
    outb(PIC_SLAVE_DATA, PIC_8086);

    /* mask the interrupts we want to receive */
    outb(PIC_MASTER_DATA, PIC_MASK_ALL); // & PIC_MASK_TIMER & PIC_MASK_KB);
    outb(PIC_SLAVE_DATA, PIC_MASK_ALL);
}

void irq_handler(struct trap_frame frame)
{
    switch (frame.trapno) {
        case IRQ_TIMER:
            printk("tick\n");
            goto out;

        case IRQ_KEYBOARD:
            printk("k\n");
            goto out;

        default:
            printk("Unhandled IRQ: %d (%s)\n", frame.trapno, VECTOR_NAME(frame.trapno));
            goto out;
    }

out:
    ack_irq(frame.trapno);
}