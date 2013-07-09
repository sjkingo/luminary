#ifndef MULTIBOOT_H
#define MULTIBOOT_H

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
