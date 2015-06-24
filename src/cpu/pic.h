#ifndef PIC_H
#define PIC_H

/* pic.h - the Intel 8259 Programmable Interrupt Controller.
 * Also contains definitions for the PIT (8253/8254) timer chip.
 */

#include "cpu/traps.h"

#define IRQ_BASE_OFFSET 32
#define IRQ_SLAVE_OFFSET (IRQ_BASE_OFFSET + 8)

enum pic_interrupts {
    IRQ_TIMER           = IRQ_BASE_OFFSET + 0,  // system timer
};

/* PIC ports */
#define PIC_MASTER_CMD  0x20
#define PIC_MASTER_DATA (PIC_MASTER_CMD+1)
#define PIC_SLAVE_CMD   0xA0
#define PIC_SLAVE_DATA  (PIC_SLAVE_CMD+1)

/* PIC data values */
#define PIC_INIT  0x10
#define PIC_ICW4  0x01
#define PIC_8086  0x01
#define PIC_EOI   0x20

/* PIC masks to enable/disable IRQ lines.
 *   to enable line:  bitwise & with current mask
 *   to disable line: bitwise | with current mask
 */
#define PIC_MASK_ALL            0xFF
#define PIC_MASK_TIMER          ~(1 << (0 & 0x7))       // IRQ 0

/* PIT (system timer) definitions.
 * See http://wiki.osdev.org/Programmable_Interval_Timer
 */
#define PIT_CH0_DATA    0x40
#define PIT_CH1_DATA    (PIT_CH0_DATA + 1)
#define PIT_CH2_DATA    (PIT_CH0_DATA + 2)
#define PIT_CMD_PORT    0x43
#define PIT_CLOCK       1193180

#define TIMER_FREQ      1000
#define TIMER_INTERVAL  (1000/TIMER_FREQ)       // ms

/* init the PIC */
void pic_init(void);

/* Called from trap_handler() when an IRQ is received */
void irq_handler(struct trap_frame);

#endif
