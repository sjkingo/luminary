#include <stdbool.h>

#include "boot/multiboot.h"
#include "cpu/pic.h"
#include "cpu/x86.h"
#include "drivers/fbdev.h"
#include "drivers/keyboard.h"
#include "drivers/mouse.h"
#include "drivers/vbe.h"
#include "drivers/vga.h"
#include "kernel/gui.h"
#include "kernel/kernel.h"
#include "kernel/syscall.h"
#include "kernel/task.h"
#include "kernel/heap.h"
#include "kernel/pmm.h"
#include "kernel/vmm.h"
#include "kernel/vfs.h"
#include "kernel/dev.h"
#include "kernel/initrd.h"
#include "pci/pci.h"

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
}

/* Print a fancy startup banner */
static void print_startup_banner(void)
{
    printk("\n");
    printk("  ..---..    \n");
    printk(" /       \\  ");
    if (!fbdev_is_ready())
        set_color(COLOR_LIGHT_BLUE, COLOR_BLACK);
    printk(" Luminary OS\n");
    if (!fbdev_is_ready())
        reset_color();
    printk("|         |  \n");
    printk("|         |  Built with: ");
    print_defines();
    printk("\n");
    printk(" \\  \\~/  /   Built at: %s %s\n", __DATE__, __TIME__);
    printk("  `, Y ,'    Kernel loaded at 0x%lx -> 0x%lx (%ld KB)\n", kernel_start, kernel_end, (kernel_end-kernel_start)/1024);
    printk("   |_|_|     \n");
    printk("   |===|     %d MB memory available\n", mem_available());
    printk("    \\_/      \n");
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
    init_pmm(mb_info, kernel_end);
    init_vmm();

    /* devices */
    init_cpu();
    init_keyboard();
    {
        struct vbe_mode_info_struct *vbe = vbe_get_mode_info();
        init_mouse(vbe ? vbe->width : 1280, vbe ? vbe->height : 960);
    }
    init_pci();

    // higher level startup
    init_task();
    init_gui();

    /* Find and parse the initrd cpio module.
     * Convention: the last multiboot module whose cmdline string contains
     * "initrd" (or any module tagged "initrd") is the cpio archive.
     * We pass it to initrd_init() which mounts it at "/".
     * Then we load init from /bin/init within the VFS. */
    if (mb_info->mods_count == 0)
        panic("no multiboot modules - cannot load initrd");

    struct multiboot_mod_entry *mods =
        (struct multiboot_mod_entry *)mb_info->mods_addr;

    /* Find the initrd module: look for one tagged "initrd", else use last */
    struct multiboot_mod_entry *initrd_mod = &mods[mb_info->mods_count - 1];
    for (uint32_t i = 0; i < mb_info->mods_count; i++) {
        const char *cmdline = (const char *)mods[i].string;
        if (cmdline) {
            /* simple substring search for "initrd" */
            const char *p = cmdline;
            while (*p) {
                if (p[0]=='i' && p[1]=='n' && p[2]=='i' && p[3]=='t' &&
                    p[4]=='r' && p[5]=='d') {
                    initrd_mod = &mods[i];
                    break;
                }
                p++;
            }
        }
    }

    uint32_t initrd_size = initrd_mod->mod_end - initrd_mod->mod_start;
    uint32_t initrd_files = initrd_init((const void *)initrd_mod->mod_start, initrd_size);
    printk("initrd: rootfs mounted at / from initrd (%ld bytes in %ld files)\n",
           initrd_size, initrd_files);
    init_devfs();

    /* Load /bin/init from the VFS */
    uint32_t init_size = 0;
    const void *init_elf = initrd_get_file("/bin/init", &init_size);
    if (!init_elf)
        panic("initrd: /bin/init not found - check rootfs/bin/init exists");
    spawn_init(init_elf, init_size);

    startup_complete = true;
    enable_interrupts();

    while(1);
}
