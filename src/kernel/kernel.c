#include <stdbool.h>

#include "boot/multiboot.h"
#include "cpu/pic.h"
#include "cpu/x86.h"
#include "drivers/vga.h"
#include "kernel/kernel.h"
#include "kernel/task.h"
#include "pci/pci.h"
#include "version.h"

struct kernel_time timekeeper;

bool startup_complete = false;

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

static void print_defines(void)
{
#ifdef DEBUG
    printk("-DDEBUG ");
#endif
#ifdef TURTLE
    printk("-DTURTLE ");
#endif
#ifdef USE_SERIAL
    printk("-DUSE_SERIAL ");
#endif
}

/* Print a fancy startup banner */
static void print_startup_banner(void)
{
    printk("  ..---..    ");
    set_color(COLOR_LIGHT_BLUE, COLOR_BLACK);
    printk("Luminary OS\n");
    reset_color();
    printk(" /       \\   Version %s\n", KERNEL_VERSION);
    printk("|         |  \n");
    printk("|         |  Built with: ");
    print_defines();
    printk("\n");
    printk(" \\  \\~/  /   Built at: %s %s\n", __DATE__, __TIME__);
    printk("  `, Y ,'    Last commit: %s\n", _GIT_LAST_COMMIT);
    printk("   |_|_|     \n");
    printk("   |===|     ");
    printk("%d MB memory available\n", mem_available());
    printk("    \\_/      ");
    printk("Timer set at %d Hz (every %d ms)\n", TIMER_FREQ, TIMER_INTERVAL);
    printk("\n");
}

extern void kernel_main() __attribute__((noreturn));
void kernel_main(struct multiboot_info *mb)
{
    disable_interrupts();

    mb_info = mb;

    init_vga(); // must be first
    print_startup_banner();
    init_cpu();
    init_pci();
    init_task();

    startup_complete = true;
    enable_interrupts();

    /*
    struct task a;
    create_task(&a, "A", 10);
    struct task b;
    create_task(&b, "B", 7);
    struct task c;
    create_task(&c, "C", 4);
    */

    while(1);
}
