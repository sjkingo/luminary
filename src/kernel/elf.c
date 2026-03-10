/* ELF loader - loads static ELF32 executables into a task's address space.
 *
 * Iterates PT_LOAD segments, allocates physical frames, copies data from
 * the ELF file, and maps pages at the specified virtual addresses with
 * PTE_USER access.
 */

#include <stdint.h>
#include <string.h>

#include "kernel/elf.h"
#include "kernel/kernel.h"
#include "kernel/pmm.h"
#include "kernel/vmm.h"

#define MODULE "elf: "

uint32_t elf_load(const void *elf_data, uint32_t elf_size, uint32_t page_dir)
{
    const struct elf32_header *ehdr = (const struct elf32_header *)elf_data;

    /* Validate ELF header */
    if (elf_size < sizeof(struct elf32_header)) {
        printk(MODULE "file too small\n");
        return 0;
    }
    if (ehdr->e_ident_magic != ELF_MAGIC) {
        printk(MODULE "bad magic: 0x%lx\n", ehdr->e_ident_magic);
        return 0;
    }
    if (ehdr->e_ident_class != 1) {
        printk(MODULE "not 32-bit ELF\n");
        return 0;
    }
    if (ehdr->e_ident_data != 1) {
        printk(MODULE "not little-endian\n");
        return 0;
    }
    if (ehdr->e_type != ET_EXEC) {
        printk(MODULE "not ET_EXEC (type=%d)\n", ehdr->e_type);
        return 0;
    }
    if (ehdr->e_machine != EM_386) {
        printk(MODULE "not i386 (machine=%d)\n", ehdr->e_machine);
        return 0;
    }

    /* Validate program header table fits in the file */
    uint32_t ph_end = ehdr->e_phoff + ehdr->e_phnum * ehdr->e_phentsize;
    if (ph_end > elf_size) {
        printk(MODULE "program headers extend past end of file\n");
        return 0;
    }

    const uint8_t *base = (const uint8_t *)elf_data;

    /* Load each PT_LOAD segment */
    for (int i = 0; i < ehdr->e_phnum; i++) {
        const struct elf32_phdr *phdr = (const struct elf32_phdr *)
            (base + ehdr->e_phoff + i * ehdr->e_phentsize);

        if (phdr->p_type != PT_LOAD)
            continue;

        /* Validate segment fits in the file */
        if (phdr->p_offset + phdr->p_filesz > elf_size) {
            printk(MODULE "segment %d extends past end of file\n", i);
            return 0;
        }

        /* Validate virtual address is in user space */
        if (phdr->p_vaddr < USER_SPACE_START ||
            phdr->p_vaddr + phdr->p_memsz > USER_SPACE_END) {
            printk(MODULE "segment %d vaddr 0x%lx outside user space\n",
                   i, phdr->p_vaddr);
            return 0;
        }

        uint32_t flags = PTE_PRESENT | PTE_USER;
        if (phdr->p_flags & PF_W)
            flags |= PTE_WRITE;

        /* Map pages for the segment. We iterate page by page. */
        uint32_t seg_start = phdr->p_vaddr & 0xFFFFF000;
        uint32_t seg_end = (phdr->p_vaddr + phdr->p_memsz + PAGE_SIZE - 1) & 0xFFFFF000;

        for (uint32_t vaddr = seg_start; vaddr < seg_end; vaddr += PAGE_SIZE) {
            uint32_t frame = pmm_alloc_frame();
            /* Identity-map so we can write to it from kernel context */
            vmm_map_page(frame, frame, PTE_PRESENT | PTE_WRITE);
            memset((void *)frame, 0, PAGE_SIZE);

            /* Copy file data that falls within this page */
            uint32_t page_off_start = vaddr;
            uint32_t page_off_end = vaddr + PAGE_SIZE;

            /* Data region of the segment (from file) */
            uint32_t data_start = phdr->p_vaddr;
            uint32_t data_end = phdr->p_vaddr + phdr->p_filesz;

            /* Overlap between this page and the file data */
            uint32_t copy_start = (page_off_start > data_start) ? page_off_start : data_start;
            uint32_t copy_end = (page_off_end < data_end) ? page_off_end : data_end;

            if (copy_start < copy_end) {
                uint32_t file_offset = phdr->p_offset + (copy_start - phdr->p_vaddr);
                uint32_t dest_offset = copy_start - vaddr;
                memcpy((void *)(frame + dest_offset),
                       base + file_offset,
                       copy_end - copy_start);
            }
            /* Remainder of page (bss) is already zeroed from memset above */

            /* Map in the task's address space */
            vmm_map_page_in(page_dir, vaddr, frame, flags);
        }

#ifdef DEBUG
        printk(MODULE "loaded segment %d: vaddr=0x%lx memsz=0x%lx filesz=0x%lx %s%s\n",
               i, phdr->p_vaddr, phdr->p_memsz, phdr->p_filesz,
               (phdr->p_flags & PF_W) ? "RW" : "R",
               (phdr->p_flags & 1) ? "X" : "");
#endif
    }

    /* Validate entry point */
    if (ehdr->e_entry < USER_SPACE_START || ehdr->e_entry >= USER_SPACE_END) {
        printk(MODULE "entry point 0x%lx outside user space\n", ehdr->e_entry);
        return 0;
    }

    printk(MODULE "entry point at 0x%lx\n", ehdr->e_entry);
    return ehdr->e_entry;
}
