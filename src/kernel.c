#include "kernel.h"
#include "multiboot.h"
#include "task.h"
#include "vga.h"
#include "x86.h"

struct kernel_time timekeeper;

void real_panic(char *msg, char const *file, int line, char const *func)
{
    set_color(COLOR_RED, COLOR_BLACK);
    printk("\n\nKernel panic at %s:%d(%s): %s\n\n", file, line, func, msg);
    reset_color();

    /* halt the CPU and spin until reset */
    extern void cpu_halt(void);
    cpu_halt(); // noreturn
    while (1); // silence compiler warning
}

extern void kernel_main() __attribute__((noreturn));
void kernel_main(struct multiboot_info *mb)
{
    disable_interrupts();

    mb_info = mb;

    init_vga(); // must be first
    printk("Luminary starting..\n");
    init_cpu();
    init_task();
    printk("available memory: %d MB\n", mem_available());

    enable_interrupts();

    struct task a;
    create_task(&a, "A", 10);
    struct task b;
    create_task(&b, "B", 7);
    struct task c;
    create_task(&c, "C", 4);

    while(1);
}
