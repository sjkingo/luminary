#ifndef PIC_H
#define PIC_H

/* pic.h - the Intel 8259 Programmable Interrupt Controller */

#include "traps.h"

#define IRQ_BASE_OFFSET 32
#define IRQ_SLAVE_OFFSET (IRQ_BASE_OFFSET + 8)

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

/* init the PIC */
void pic_init(void);

/* Called from trap_handler() when an IRQ is received */
void irq_handler(struct trap_frame);

#endif
