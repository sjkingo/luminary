#include "kernel.h"
#include "pic.h"
#include "task.h"
#include "traps.h"
#include "x86.h"

static inline void ack_irq(int irq)
{
    if (irq >= IRQ_SLAVE_OFFSET) {
        printk("ACK'ing slave as irq=%d and IRQ_SLAVE_OFFSET=%d\n", irq, IRQ_SLAVE_OFFSET);
        outb(PIC_SLAVE_CMD, PIC_EOI);
    }
    outb(PIC_MASTER_CMD, PIC_EOI);
}

static void timer_init(void)
{
    unsigned int div = PIT_CLOCK / TIMER_FREQ;
    outb(PIT_CMD_PORT, 0x36); /* command byte */
    outb(PIT_CH0_DATA, (unsigned char)(div & 0xFF));
    outb(PIT_CH0_DATA, (unsigned char)((div >> 8) & 0xFF));
    timekeeper.uptime_ms = 0;
#ifdef TURTLE
    printk("Build with TURTLE: will only schedule every second\n");
#endif
}

static void timer_tick(void)
{
    timekeeper.uptime_ms += TIMER_INTERVAL;
#ifdef TURTLE
    /* Only run scheduler every second */
    if ((timekeeper.uptime_ms % TIMER_FREQ) == 0)
        sched();
#else
    sched();
#endif
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

    /* Set up the PIT chip here, before masking PIC_MASK_TIMER, otherwise we
     * will get spurious timer interrupts at a BIOS-set frequency.  Note this
     * really shouldn't matter because interrupts will already be disabled!
     */
    timer_init();

    /* mask the interrupts we want to receive */
    outb(PIC_MASTER_DATA, PIC_MASK_ALL & PIC_MASK_TIMER);
    outb(PIC_SLAVE_DATA, PIC_MASK_ALL);
}

void irq_handler(struct trap_frame frame)
{
    switch (frame.trapno) {
        case IRQ_TIMER:
            timer_tick();
            goto out;

        default:
            printk("Unhandled IRQ%d\n", frame.trapno);
            goto out;
    }

out:
    ack_irq(frame.trapno);
}
