#include <stdbool.h>

#include "boot/multiboot.h"
#include "cpu/pic.h"
#include "cpu/x86.h"
#include "drivers/fbdev.h"
#include "drivers/vbe.h"
#include "drivers/vga.h"
#include "kernel/kernel.h"
#include "kernel/syscall.h"
#include "kernel/task.h"
#include "kernel/heap.h"
#include "kernel/pmm.h"
#include "kernel/vmm.h"
#include "pci/pci.h"
#include "version.h"

struct multiboot_info *mb_info;
struct kernel_time timekeeper;

bool startup_complete = false;

// kernel segment addresses
static uint32_t kernel_start;
static uint32_t kernel_stack;
static uint32_t kernel_end;

void real_panic(char *msg, char const *file, int line, char const *func)
{
    if (!fbdev_is_ready())
        set_color(COLOR_RED, COLOR_BLACK);
    printk("\n\nKernel panic at %s:%d(%s): %s\n\n", file, line, func, msg);
    if (!fbdev_is_ready())
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
    if (!fbdev_is_ready())
        set_color(COLOR_LIGHT_BLUE, COLOR_BLACK);
    printk("Luminary OS\n");
    if (!fbdev_is_ready())
        reset_color();
    printk(" /       \\   Version %s\n", KERNEL_VERSION);
    printk("|         |  \n");
    printk("|         |  Built with: ");
    print_defines();
    printk("\n");
    printk(" \\  \\~/  /   Built at: %s %s\n", __DATE__, __TIME__);
    printk("  `, Y ,'    Kernel loaded at 0x%lx -> 0x%lx (%ld KB)\n", kernel_start, kernel_end, (kernel_end-kernel_start)/1024);
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
        printk("0x%lx -> 0x%lx (%ld KB)\n", mmap->base_addr_low,
                (mmap->base_addr_low + mmap->length_low), mmap->length_low / 1024);
		mmap = (struct multiboot_memory_map *)((unsigned long)mmap + mmap->size + sizeof(mmap->size));
	}
}

/* Test tasks for context switching */
static struct task task_a, task_b;

static void task_a_func(void)
{
    while (1)
        ;
}

/* User-mode program for taskB.
 * This is position-independent x86 code that will be copied to USER_SPACE_START.
 * It writes "user!\n" via sys_write (int 0x80, EAX=1, EBX=buf, ECX=len),
 * then loops forever.
 *
 * Equivalent to:
 *   push 0x0a21     ; "!\n" on stack
 *   push 0x72657375 ; "user" on stack
 *   mov eax, 1      ; SYS_WRITE
 *   mov ebx, esp    ; buffer = stack pointer
 *   mov ecx, 6      ; length
 *   int 0x80
 *   jmp $           ; spin forever
 */
static const unsigned char user_program[] = {
    0x68, 0x21, 0x0a, 0x00, 0x00,       /* push 0x00000a21  "!\n\0\0" */
    0x68, 0x75, 0x73, 0x65, 0x72,       /* push 0x72657375  "user"    */
    0xB8, 0x01, 0x00, 0x00, 0x00,       /* mov eax, 1  (SYS_WRITE)   */
    0x89, 0xE3,                         /* mov ebx, esp               */
    0xB9, 0x06, 0x00, 0x00, 0x00,       /* mov ecx, 6                */
    0xCD, 0x80,                         /* int 0x80                   */
    0xEB, 0xFE,                         /* jmp $ (infinite loop)      */
};

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
    printk("kernel stack at 0x%lx\n", kernel_stack);

    print_startup_banner();

    /* physical and virtual memory */
    init_pmm(mb_info->mem_upper);
    init_vmm();

    /* devices */
    init_cpu();
    init_pci();

    // higher level startup
    init_task();
    create_task(&task_a, "taskA", 5, task_a_func);
    create_user_task(&task_b, "taskB", 3,
                     (void *)user_program, sizeof(user_program));

    startup_complete = true;
    enable_interrupts();

    while(1);
}
