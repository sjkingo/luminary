#include "kernel.h"
#include "gdt.h"
#include "x86.h"

struct gdt_entry gdt[GDT_NUM_ENTRIES];
struct gdt_ptr gptr;

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

void init_cpu(void)
{
    if (!in_protected_mode()) {
        panic("cpu is not in protected mode!");
    }

    gdt_install();
}
