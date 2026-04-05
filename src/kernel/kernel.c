#include <stdbool.h>

#include "boot/multiboot.h"
#include "cpu/pic.h"
#include "cpu/x86.h"
#include "drivers/ata.h"
#include "drivers/blkdev.h"
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
#include "kernel/sys_dev.h"
#include "kernel/cmdline.h"
#include "kernel/initrd.h"
#include "kernel/tmpfs.h"
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
    const char *cl = cmdline_raw();
    if (cl && cl[0])
        printk("   |===|     cmdline: %s\n", cl);
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

    /* Parse kernel cmdline early — banner and boot path both need it */
    const char *cmdline_str = (mb_info->flags & (1 << 2))
        ? (const char *)mb_info->cmdline : NULL;
    cmdline_init(cmdline_str);

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

    const char *root_dev = cmdline_get("root");

    if (!root_dev) {
        /* No root= on cmdline — boot from initrd */
        if (mb_info->mods_count == 0)
            panic("no multiboot modules and no root= cmdline - cannot boot");

        struct multiboot_mod_entry *mods =
            (struct multiboot_mod_entry *)mb_info->mods_addr;

        /* Find the initrd module: look for one tagged "initrd", else use last */
        struct multiboot_mod_entry *initrd_mod = &mods[mb_info->mods_count - 1];
        for (uint32_t i = 0; i < mb_info->mods_count; i++) {
            const char *modcmd = (const char *)mods[i].string;
            if (modcmd) {
                const char *p = modcmd;
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
        uint32_t initrd_files = 0;
        struct vfs_node *initrd_root = initrd_init(
            (const void *)initrd_mod->mod_start, initrd_size, &initrd_files);
        if (!initrd_root)
            panic("initrd: failed to parse cpio archive");

        vfs_set_root(initrd_root);
        vfs_mount("/", "initrd", initrd_root);
        initrd_root->readonly = true;

        init_tmpfs();
        struct vfs_node *tmp_dir = vfs_lookup("/tmp");
        if (!tmp_dir) {
            tmp_dir = vfs_alloc_node();
            if (!tmp_dir) panic("initrd: out of VFS nodes for /tmp");
            tmp_dir->name[0] = 't'; tmp_dir->name[1] = 'm';
            tmp_dir->name[2] = 'p'; tmp_dir->name[3] = '\0';
            tmp_dir->flags   = VFS_DIR;
            vfs_add_child(initrd_root, tmp_dir);
        }
        if (vfs_do_mount("/tmp", "tmpfs") != 0)
            panic("initrd: failed to mount /tmp as tmpfs");

        printk("initrd: rootfs mounted at / (%ld bytes, %ld files)\n",
               initrd_size, initrd_files);

        init_devfs();
        init_dev_sys();
        init_dev_x();
        init_ata();

        uint32_t init_size = 0;
        const void *init_elf = initrd_get_file("/bin/init", &init_size);
        if (!init_elf)
            panic("initrd: /bin/init not found");
        spawn_init(init_elf, init_size);
    } else {
        /* root= specified — mount block device as root (requires ext2 driver) */
        init_devfs();
        init_dev_sys();
        init_dev_x();
        init_ata();

        /* Strip /dev/ prefix if present */
        const char *devname = root_dev;
        if (devname[0]=='/' && devname[1]=='d' && devname[2]=='e' &&
            devname[3]=='v' && devname[4]=='/') devname += 5;

        struct blkdev *bd = blkdev_find(devname);
        if (!bd) {
            printk("root: block device '%s' not found\n", root_dev);
            panic("root: block device not found - check root= cmdline");
        }

        printk("root: device %s found (%ld sectors)\n",
               bd->name, bd->sector_count);

        panic("root: ext2 driver not yet implemented - cannot mount rootfs");
    }

    startup_complete = true;
    enable_interrupts();

    while(1);
}
