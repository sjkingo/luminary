#include <stdbool.h>

#include "boot/multiboot.h"
#include "cpu/pic.h"
#include "cpu/x86.h"
#include "drivers/vbe.h"
#include "drivers/vga.h"
#include "kernel/kernel.h"
#include "kernel/task.h"
#include "kernel/heap.h"
#include "pci/pci.h"
#include "version.h"

struct kernel_time timekeeper;

bool startup_complete = false;

uint32_t kernel_start;
uint32_t kernel_stack;
uint32_t kernel_end;

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
    printk("  `, Y ,'    Kernel loaded at 0x%x -> 0x%x (%d KB)\n", kernel_start, kernel_end, (kernel_end-kernel_start)/1024);
    printk("   |_|_|     \n");
    printk("   |===|     ");
    printk("%d MB memory available\n", mem_available());
    printk("    \\_/      ");
    printk("Timer set at %d Hz (every %d ms)\n", TIMER_FREQ, TIMER_INTERVAL);
    printk("\n");
}

static void print_memory_map(void)
{
    printk("BIOS memory map:\n");
    struct multiboot_memory_map *mmap = (struct multiboot_memory_map *)mb_info->mmap_addr;
    while((unsigned long)mmap < mb_info->mmap_addr + mb_info->mmap_length) {
        switch (mmap->type) {
            case MEMORY_TYPE_RAM:
                printk("  usable RAM\t");
                break;
            case MEMORY_TYPE_RESERVED:
                printk("  reserved\t");
                break;
            case MEMORY_TYPE_ACPI:
            case MEMORY_TYPE_NVS:
                printk("  ACPI-mapped\t");
                break;
            case MEMORY_TYPE_UNUSABLE:
                printk("  unusable\t");
                break;
            default:
                printk("  ?????\t");
        }
        printk("0x%x -> 0x%x (%d KB)\n", mmap->base_addr_low,
                (mmap->base_addr_low + mmap->length_low), mmap->length_low / 1024);
		mmap = (struct multiboot_memory_map *)((unsigned long)mmap + mmap->size + sizeof(mmap->size));
	}
}

extern void kernel_main() __attribute__((noreturn));
void kernel_main(struct multiboot_info *mb, uint32_t start, uint32_t stack, uint32_t end)
{
    disable_interrupts();

    mb_info = mb;
    kernel_start = start;
    kernel_stack = stack;
    kernel_end = end;

    // init early graphics
    init_vga(); // must be first
    init_vbe(mb); // 2nd

    // set up kernel heap 1M above the end of the kernel
    print_memory_map();
    init_kernel_heap((void *)(kernel_end + INT_1M));
    printk("kernel stack at 0x%x\n", kernel_stack);

    print_startup_banner();

    /* devices */
    init_cpu();
    init_pci();

    // higher level startup
    init_task();

    startup_complete = true;
    enable_interrupts();

    while(1);
}
