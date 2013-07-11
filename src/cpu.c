#include "kernel.h"
#include "dt.h"
#include "traps.h"
#include "x86.h"

/* The GDT */
struct gdt_entry gdt[GDT_NUM_ENTRIES];
struct gdt_ptr gptr;

/* The IDT */
struct idt_entry idt[256];
struct idt_ptr iptr;

static inline void gdt_set_gate(int index, unsigned int base, unsigned int limit,
        unsigned char access, unsigned char granularity)
{
    gdt[index].base_low = (base & 0xFFFF);
    gdt[index].base_middle = (base >> 16) & 0xFF;
    gdt[index].base_high = (base >> 24) & 0xFF;
    gdt[index].limit_low = (limit & 0xFFFF);
    gdt[index].access = access;
    gdt[index].granularity |= (granularity & 0xF0);
}

static void gdt_install(void)
{
    gptr.limit = (sizeof(struct gdt_entry) * GDT_NUM_ENTRIES) - 1;
    gptr.base = (unsigned int)&gdt;

    /* Access and granularity bitmasks are described at http://www.osdever.net/bkerndev/Docs/gdt.htm
     *
     * access definitions are in x86.h
     * granularity of 0xCF = 32 bit, 4KB->4GB
     *
     *              32 bits   32 bits     8 bits                8 bits
     *           i  base      limit       access                granularity     */
    gdt_set_gate(0, 0x0,      0x0,        0,                    0);
    gdt_set_gate(1, 0x0,      0xFFFFFFFF, GDT_GATE_KERNEL_CODE, 0xCF);
    gdt_set_gate(2, 0x0,      0xFFFFFFFF, GDT_GATE_KERNEL_DATA, 0xCF);

    extern void gdt_flush(void);
    gdt_flush();

    printk("Installed GDT at address %X\n", (unsigned int)&gptr);
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
    printk("Installed IDT\n");
}

void init_cpu(void)
{
    if (!in_protected_mode()) {
        panic("cpu is not in protected mode!");
    }

    gdt_install();
    idt_install();
}

void trap_handler(struct trap_frame frame)
{
    /* dump trap frame */
    printk("\n");
    printk("Unhandled exception: %d (%s) at %08x\n", frame.trapno, VECTOR_NAME(frame.trapno), frame.eip);
    printk("ERR=%04x IP=%04x:%08x SP=%04x:%08x GDT=%08x IDT=%08x\n", frame.err, 
            frame.cs, frame.eip, frame.ds, frame.esp, &gptr, &iptr);
    if (frame.magic != TRAP_MAGIC)
        printk("BUG: magic=0x%x INVALID -- frame may be corrupt.\n", frame.magic);
    printk("EAX=%08x EBX=%08x ECX=%08x EDX=%08x [u]ESP=%08x\n", frame.eax, frame.ebx, frame.ecx, frame.edx, frame.uesp);
    printk("ESI=%08x EDI=%08x EBP=%08x ESP=%08x [u]SS =%08x\n", frame.esi, frame.edi, frame.ebp, frame.esp, frame.uss);
    printk("DS=%04x CS=%04x EFLAGS=[ ", frame.ds, frame.cs);
    for (int i = 0; i < 22; i++) {
        if (i == 1 || i == 3 || i == 5 || (i >= 12 && i <= 18)) {
            // reserved or obseleted
            printk("-");
            continue;
        }
        if (frame.eflags & (1<<i)) {
            printk("1");
        } else {
            printk("0");
        }
    }
    printk(" ] PE=%d PG=%d\n", in_protected_mode(), is_paging_enabled());
    printk("\n");

    // frame->cs should be 0x8 (KERNEL_SEG), which means frame->{esp,ss} are undefined
    if (frame.cs != IDT_KERNEL_SEG) {
        printk("BUG: trap received from outside KERNEL_SEG (0x%x) or invalid frame\n", frame.cs);
    }

    // we can't recover from a GPF
    if (frame.trapno == 13) {
        panic("general protection fault");
    }
}
