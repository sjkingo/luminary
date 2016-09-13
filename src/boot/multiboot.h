#pragma once

/* Multiboot header definitions used in boot.S. See:
 * https://www.gnu.org/software/grub/manual/multiboot/multiboot.html#Header-magic-fields
 */
#define MULTIBOOT_MAGIC  0x1badb002
#define MULTIBOOT_FLAG_ALIGN    (1<<0)
#define MULTIBOOT_FLAG_MEM      (1<<1)
#define MULTIBOOT_FLAG_GFX      (1<<2)
#define MULTIBOOT_FLAGS         MULTIBOOT_FLAG_ALIGN | MULTIBOOT_FLAG_MEM | MULTIBOOT_FLAG_GFX
#define MULTIBOOT_CHECKSUM      -(MULTIBOOT_MAGIC + MULTIBOOT_FLAGS)

#ifndef __ASSEMBLER__

/* External definitions in vbe.h */
struct vbe_control_struct;
struct vbe_mode_struct;

struct multiboot_aout_table {
    unsigned long tabsize;
    unsigned long strsize;
    unsigned long addr;
    unsigned long reserved;
};

struct multiboot_elf_table {
    unsigned long num;
    unsigned long size;
    unsigned long addr;
    unsigned long shndx;
};

struct multiboot_info {
    unsigned long flags;
    unsigned long mem_lower;
    unsigned long mem_upper;
    unsigned long boot_device;
    unsigned long cmdline;
    unsigned long mods_count;
    unsigned long mods_addr;
    union {
        struct multiboot_aout_table aout_sym;
        struct multiboot_elf_table elf_sym;
    };
    unsigned long mmap_length;
    unsigned long mmap_addr;

    unsigned long drives_length;
    unsigned long drives_addr;

    unsigned long config_table;

    unsigned long boot_loader_name;

    unsigned long apm_table;

    struct vbe_control_struct *vbe_control_info;
    struct vbe_mode_info_struct *vbe_mode_info;
    unsigned long vbe_mode;
    unsigned long vbe_interface_seg;
    unsigned long vbe_interface_off;
    unsigned long vbe_interface_len;
};

extern struct multiboot_info *mb_info;

static inline int mem_available(void)
{
    /* from Multiboot: "The value returned for upper memory is maximally the
     * address of the first upper memory hole minus 1 megabyte"
     */
    return (mb_info->mem_upper / 1024) + 1;
}

#endif
