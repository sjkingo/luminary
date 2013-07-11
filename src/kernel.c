#include "kernel.h"
#include "multiboot.h"
#include "vga.h"

void panic(char *msg)
{
    printk("\n\nKernel panic: %s\n\n", msg);

    /* halt the CPU and spin until reset */
    extern void cpu_halt(void);
    cpu_halt(); // noreturn
    while (1); // silence compiler warning
}

extern void kernel_main() __attribute__((noreturn));
void kernel_main(struct multiboot_info *mb)
{
    mb_info = mb;

    init_vga(); // must be first
    printk("Luminary starting..\n");

    init_cpu();

    printk("available memory: %d MB\n", mem_available());

    while(1);
}
