#include "kernel.h"
#include "multiboot.h"
#include "vga.h"

void kernel_main(struct multiboot_info *mb)
{
    mb_info = mb;

    init_vga(); // must be first
    printk("Luminary starting..\n");

    init_cpu();

    printk("available memory: %d MB\n", mem_available());

    while(1);
}
