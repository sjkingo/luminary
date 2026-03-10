#include <string.h>

#include "cpu/dt.h"
#include "cpu/pic.h"
#include "cpu/traps.h"
#include "cpu/x86.h"
#include "kernel/kernel.h"

#ifdef USE_SERIAL
#include "drivers/serial.h"
#endif

/* The GDT */
struct gdt_entry gdt[GDT_NUM_ENTRIES];
struct gdt_ptr gptr;

/* The IDT */
struct idt_entry idt[256];
struct idt_ptr iptr;

/* The TSS */
static struct tss_entry tss;

static inline void gdt_set_gate(int index, unsigned int base, unsigned int limit,
        unsigned char access, unsigned char granularity)
{
    gdt[index].base_low = (base & 0xFFFF);
    gdt[index].base_middle = (base >> 16) & 0xFF;
    gdt[index].base_high = (base >> 24) & 0xFF;
    gdt[index].limit_low = (limit & 0xFFFF);
    gdt[index].granularity = (limit >> 16) & 0x0F;
    gdt[index].granularity |= (granularity & 0xF0);
    gdt[index].access = access;
}

static void write_tss(int index, uint32_t ss0, uint32_t esp0)
{
    uint32_t base = (uint32_t)&tss;
    uint32_t limit = base + sizeof(tss) - 1;

    /* TSS descriptor: present, ring 3, type 0x9 (available 32-bit TSS) */
    gdt_set_gate(index, base, limit, 0xE9, 0x00);

    memset(&tss, 0, sizeof(tss));
    tss.ss0 = ss0;
    tss.esp0 = esp0;

    /* Segment selectors the CPU loads when returning to ring 3.
     * These are kernel selectors with RPL=3 set in the low 2 bits. */
    tss.cs = SEG_KERNEL_CODE | 0x3;
    tss.ss = SEG_KERNEL_DATA | 0x3;
    tss.ds = SEG_KERNEL_DATA | 0x3;
    tss.es = SEG_KERNEL_DATA | 0x3;
    tss.fs = SEG_KERNEL_DATA | 0x3;
    tss.gs = SEG_KERNEL_DATA | 0x3;

    /* I/O map base points past the TSS to deny all port access from ring 3 */
    tss.iomap_base = sizeof(tss);
}

void tss_set_kernel_stack(uint32_t esp0)
{
    tss.esp0 = esp0;
}

static void gdt_install(void)
{
    gptr.limit = (sizeof(struct gdt_entry) * GDT_NUM_ENTRIES) - 1;
    gptr.base = (unsigned int)&gdt;

    /* Access and granularity bitmasks are described at http://www.osdever.net/bkerndev/Docs/gdt.htm
     *
     * Granularity of 0xCF = 32 bit, 4KB granularity, 0-4GB
     *
     *              32 bits   32 bits     8 bits                8 bits
     *           i  base      limit       access                granularity     */
    gdt_set_gate(0, 0x0,      0x0,        0,                    0);
    gdt_set_gate(1, 0x0,      0xFFFFFFFF, GDT_GATE_KERNEL_CODE, 0xCF);
    gdt_set_gate(2, 0x0,      0xFFFFFFFF, GDT_GATE_KERNEL_DATA, 0xCF);
    gdt_set_gate(3, 0x0,      0xFFFFFFFF, GDT_GATE_USER_CODE,   0xCF);
    gdt_set_gate(4, 0x0,      0xFFFFFFFF, GDT_GATE_USER_DATA,   0xCF);
    write_tss(5, SEG_KERNEL_DATA, 0x0);

    extern void gdt_flush(void);
    extern void tss_flush(void);
    gdt_flush();
    tss_flush();

#ifdef DEBUG
    printk("Installed GDT at %08x (%d entries, TSS at %08lx)\n",
           (unsigned int)&gptr, GDT_NUM_ENTRIES, (uint32_t)&tss);
#endif
}

static inline void idt_set_gate(int index, int vector, int selector, char flags)
{
    idt[index].base_low = vector & 0xFFFF;
    idt[index].base_high = (vector >> 16) & 0xFFFF;
    idt[index].selector = selector;
    idt[index].always0 = 0;
    idt[index].flags = flags | 0x60; /* ring 0 */
}

extern int vectors[]; /* in vectors.s */
static void idt_install(void)
{
    iptr.limit = (sizeof(struct idt_entry) * NUM_TRAP_VECTORS) - 1;
    iptr.base = (unsigned int)&idt;

    /* fill in the IDT */
    for (int i = 0; i < NUM_TRAP_VECTORS; i++) {
        idt_set_gate(i, vectors[i], IDT_KERNEL_SEG, 0x8E); /* TODO: 0x8E ??? */
    }

    asm volatile("lidt (%0)" : : "r" (&iptr));
#ifdef DEBUG
    printk("Installed IDT at %08x\n", (unsigned int)&iptr);
#endif
}

void init_cpu(void)
{
    if (!in_protected_mode()) {
        panic("cpu is not in protected mode");
    }

    gdt_install();
    idt_install();
    pic_init();
#ifdef USE_SERIAL
    serial_init();
#endif
}

void dump_trap_frame(struct trap_frame *frame)
{
    printk("\n");
    if (frame->magic != TRAP_MAGIC) {
        printk("BUG: magic=0x%x INVALID -- trap frame may be corrupt.\n", frame->magic);
        return;
    }
    printk("Unhandled exception: %d (%s) at %08x\n", frame->trapno, VECTOR_NAME(frame->trapno), frame->eip);
    printk("ERR=%04x IP=%04x:%08x SP=%04x:%08x GDT=%08x IDT=%08x\n", frame->err,
            frame->cs, frame->eip, frame->ds, frame->esp, (unsigned int)&gptr, (unsigned int)&iptr);
    printk("EAX=%08x EBX=%08x ECX=%08x EDX=%08x [u]ESP=%08x\n", frame->eax, frame->ebx, frame->ecx, frame->edx, frame->uesp);
    printk("ESI=%08x EDI=%08x EBP=%08x ESP=%08x [u]SS =%08x\n", frame->esi, frame->edi, frame->ebp, frame->esp, frame->uss);
    printk("DS=%04x CS=%04x EFLAGS=[ ", frame->ds, frame->cs);
    for (int i = 0; i < 22; i++) {
        if (i == 1 || i == 3 || i == 5 || (i >= 12 && i <= 18)) {
            // reserved or obseleted
            printk("-");
            continue;
        }
        if (frame->eflags & (1<<i)) {
            printk("1");
        } else {
            printk("0");
        }
    }
    printk(" ] PE=%d PG=%d\n", in_protected_mode(), is_paging_enabled());
    printk("\n");
}

void trap_handler(struct trap_frame *frame)
{
    if (frame->trapno >= IRQ_BASE_OFFSET) {
        /* interrupt from the PIC */
        irq_handler(frame);
        goto out;
    } else {
        /* CPU exception */
        goto exc_handler;
    }

exc_handler:
    dump_trap_frame(frame);

    /* frame->cs should always be == IDT_KERNEL_SEG */
    if (frame->cs != IDT_KERNEL_SEG) {
        printk("BUG: trap received from outside kernel code segment (CS=%04x)\n", frame->cs);
        printk("     (will attempt to continue execution..)\n");
    }

    /* handle the trap - most will be a panic() */
    switch (frame->trapno) {
        case INT_DEBUG:
        case INT_BREAK:
            printk("STOP: %s detected\n", VECTOR_NAME(frame->trapno));
            panic("Stopping kernel execution as requested");

        case INT_PAGE_FAULT: {
            unsigned int fault_addr;
            asm volatile("mov %%cr2, %0" : "=r"(fault_addr));
            printk("Page fault at 0x%x (err=0x%x)\n", fault_addr, frame->err);
            if (!(frame->err & 0x1)) printk("  page not present\n");
            if (frame->err & 0x2)    printk("  write access\n");
            if (frame->err & 0x4)    printk("  user mode\n");
            panic("page fault");
        }

        default:
            panic("CPU exception");
    }

out:
    return;
}
