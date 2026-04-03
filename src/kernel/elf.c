/* ELF loader - loads static ELF32 executables into a task's address space.
 *
 * Iterates PT_LOAD segments, allocates physical frames, copies data from
 * the ELF file, and maps pages at the specified virtual addresses with
 * PTE_USER access. Sets up argc/argv on the user stack.
 */

#include <stdint.h>
#include <string.h>

#include "kernel/elf.h"
#include "kernel/kernel.h"
#include "kernel/pmm.h"
#include "kernel/vmm.h"

#define MODULE "elf: "

/* Walk page_dir (physical address) to find the physical frame backing vaddr.
 *
 * Both the page directory frame and its page table frames are identity-mapped
 * in the kernel page directory: the directory frame by vmm_create_page_dir(),
 * and each page table frame by vmm_map_page_in() at the time it was allocated.
 * So we can access them directly through their physical addresses. */
static uint32_t resolve_uva(uint32_t dir_phys, uint32_t vaddr,
                            uint32_t *out_frame, uint32_t *out_off)
{
    uint32_t pdi = vaddr >> 22;
    uint32_t pti = (vaddr >> 12) & 0x3FF;

    DBGK("elf", "  resolve_uva: dir_phys=0x%lx vaddr=0x%lx pdi=%lu pti=%lu\n",
         dir_phys, vaddr, pdi, pti);
    uint32_t pde = ((uint32_t *)dir_phys)[pdi];
    DBGK("elf", "  resolve_uva: pde=0x%lx\n", pde);
    uint32_t pt_phys = pde & 0xFFFFF000;
    DBGK("elf", "  resolve_uva: pt_phys=0x%lx -- about to read pte\n", pt_phys);
    uint32_t pte = ((uint32_t *)pt_phys)[pti];
    DBGK("elf", "  resolve_uva: pte=0x%lx\n", pte);
    uint32_t frame = pte & 0xFFFFF000;

    *out_frame = frame;
    *out_off   = vaddr & 0xFFF;
    return frame;
}

uint32_t elf_load(const void *elf_data, uint32_t elf_size, uint32_t page_dir,
                  int argc, const char *const *argv, uint32_t *out_sp)
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

        if (phdr->p_offset + phdr->p_filesz > elf_size) {
            printk(MODULE "segment %d extends past end of file\n", i);
            return 0;
        }

        if (phdr->p_vaddr < USER_SPACE_START ||
            phdr->p_vaddr + phdr->p_memsz > USER_SPACE_END) {
            printk(MODULE "segment %d vaddr 0x%lx outside user space\n",
                   i, phdr->p_vaddr);
            return 0;
        }

        uint32_t flags = PTE_PRESENT | PTE_USER;
        if (phdr->p_flags & PF_W)
            flags |= PTE_WRITE;

        uint32_t seg_start = phdr->p_vaddr & 0xFFFFF000;
        uint32_t seg_end = (phdr->p_vaddr + phdr->p_memsz + PAGE_SIZE - 1) & 0xFFFFF000;

        DBGK("elf", "segment %d: vaddr=0x%lx memsz=0x%lx filesz=0x%lx flags=0x%lx\n",
             i, phdr->p_vaddr, phdr->p_memsz, phdr->p_filesz, (uint32_t)phdr->p_flags);

        for (uint32_t vaddr = seg_start; vaddr < seg_end; vaddr += PAGE_SIZE) {
            uint32_t frame = pmm_alloc_frame();
            vmm_map_page(frame, frame, PTE_PRESENT | PTE_WRITE);
            memset((void *)frame, 0, PAGE_SIZE);

            uint32_t page_off_start = vaddr;
            uint32_t page_off_end = vaddr + PAGE_SIZE;
            uint32_t data_start = phdr->p_vaddr;
            uint32_t data_end = phdr->p_vaddr + phdr->p_filesz;

            uint32_t copy_start = (page_off_start > data_start) ? page_off_start : data_start;
            uint32_t copy_end = (page_off_end < data_end) ? page_off_end : data_end;

            if (copy_start < copy_end) {
                uint32_t file_offset = phdr->p_offset + (copy_start - phdr->p_vaddr);
                uint32_t dest_offset = copy_start - vaddr;
                memcpy((void *)(frame + dest_offset),
                       base + file_offset,
                       copy_end - copy_start);
            }

            /* Leave frame identity-mapped in the kernel directory so that
             * vmm_clone_page_dir() can read it later without faulting. */
            vmm_map_page_in(page_dir, vaddr, frame, flags);
            DBGK("elf", "  mapped vaddr=0x%lx -> frame=0x%lx\n", vaddr, frame);
        }
    }

    /* Validate entry point */
    if (ehdr->e_entry < USER_SPACE_START || ehdr->e_entry >= USER_SPACE_END) {
        printk(MODULE "entry point 0x%lx outside user space\n", ehdr->e_entry);
        return 0;
    }

    /* Allocate USER_STACK_PAGES pages of user stack.
     * USER_STACK_TOP is the base of the top page (0xBFFFF000), so we
     * allocate pages from there downward: page 0 covers
     * [USER_STACK_TOP .. USER_STACK_TOP+PAGE_SIZE), page 1 covers
     * [USER_STACK_TOP-PAGE_SIZE .. USER_STACK_TOP), etc.
     * The highest usable address is then USER_STACK_TOP+PAGE_SIZE-4. */
    uint32_t stack_top = USER_STACK_TOP + PAGE_SIZE; /* one past top page base */
    for (int i = 0; i < USER_STACK_PAGES; i++) {
        uint32_t stack_frame = pmm_alloc_frame();
        vmm_map_page(stack_frame, stack_frame, PTE_PRESENT | PTE_WRITE);
        memset((void *)stack_frame, 0, PAGE_SIZE);
        /* Leave stack_frame identity-mapped in the kernel directory. */
        uint32_t stack_vaddr = stack_top - (uint32_t)(i + 1) * PAGE_SIZE;
        vmm_map_page_in(page_dir, stack_vaddr, stack_frame,
                        PTE_PRESENT | PTE_WRITE | PTE_USER);
        DBGK("elf", "  stack[%d] vaddr=0x%lx -> frame=0x%lx\n", i, stack_vaddr, stack_frame);
    }

    /* Build argc/argv on the user stack.
     * We write into a kernel-accessible scratch buffer, then copy it
     * to the stack frames we just allocated.
     *
     * Layout (stack grows down, ESP ends up pointing at the dummy retaddr):
     *   [strings area]   <- argv string data packed here
     *   [argv[0..argc-1] pointers + NULL terminator]
     *   [argc]
     *   [dummy retaddr]  <- initial ESP
     *
     * _start is compiled as a normal C function (push ebp / mov ebp,esp),
     * so after the prologue: [ebp+8]=argc, [ebp+0xc]=argv.
     */

    /* Use a 4KB scratch buffer (enough for 16 args + strings) */
    static uint8_t scratch[4096];
    uint32_t scratch_top = sizeof(scratch);  /* offset from base, grows down */

    /* Copy strings into scratch from the top */
    uint32_t str_offsets[32]; /* offsets within scratch where each string lands */
    int n = (argc > 32) ? 32 : argc;
    for (int i = n - 1; i >= 0; i--) {
        const char *s = argv[i];
        uint32_t slen = 0;
        while (s[slen]) slen++;
        slen++; /* include NUL */
        scratch_top -= slen;
        scratch_top &= ~3u; /* align to 4 bytes */
        memcpy(scratch + scratch_top, s, slen);
        str_offsets[i] = scratch_top;
    }

    /* Compute where the scratch buffer will land in user virtual space.
     * scratch[scratch_top..sizeof(scratch)] is the used portion. */
    uint32_t scratch_used = (uint32_t)sizeof(scratch) - scratch_top;
    uint32_t strings_vaddr = stack_top - scratch_used;
    strings_vaddr &= ~3u;

    /* argv pointer array below the strings, then argc, then dummy retaddr */
    uint32_t argv_vaddr = strings_vaddr - (uint32_t)(n + 1) * 4;
    argv_vaddr &= ~3u;
    uint32_t argc_vaddr = argv_vaddr - 4;
    uint32_t retaddr_vaddr = argc_vaddr - 4;

    DBGK("elf", "stack_top=0x%lx strings_vaddr=0x%lx argv_vaddr=0x%lx "
         "argc_vaddr=0x%lx retaddr_vaddr=0x%lx\n",
         stack_top, strings_vaddr, argv_vaddr, argc_vaddr, retaddr_vaddr);

    /* Helper macro: write one uint32_t to user vaddr through page_dir */
#define WRITE_USER32(dir_phys_arg, vaddr_arg, val_arg) do { \
    uint32_t _frame, _off; \
    DBGK("elf", "WRITE_USER32: vaddr=0x%lx val=0x%lx\n", \
         (uint32_t)(vaddr_arg), (uint32_t)(val_arg)); \
    resolve_uva((dir_phys_arg), (vaddr_arg), &_frame, &_off); \
    vmm_map_page(_frame, _frame, PTE_PRESENT | PTE_WRITE); \
    *(uint32_t *)(_frame + _off) = (uint32_t)(val_arg); \
    vmm_unmap_page(_frame); \
} while(0)

    /* Write the string data to the user stack pages */
    DBGK("elf", "writing strings: scratch_used=%lu\n", scratch_used);
    if (scratch_used > 0) {
        uint32_t remaining = scratch_used;
        uint32_t src_off = scratch_top;
        uint32_t dst_virt = strings_vaddr;

        while (remaining > 0) {
            uint32_t frame, off;
            DBGK("elf", "string copy: dst_virt=0x%lx\n", dst_virt);
            resolve_uva(page_dir, dst_virt & 0xFFFFF000, &frame, &off);
            uint32_t page_off = dst_virt & 0xFFF;

            vmm_map_page(frame, frame, PTE_PRESENT | PTE_WRITE);
            uint32_t can_write = PAGE_SIZE - page_off;
            if (can_write > remaining) can_write = remaining;
            memcpy((void *)(frame + page_off), scratch + src_off, can_write);
            vmm_unmap_page(frame);

            dst_virt += can_write;
            src_off += can_write;
            remaining -= can_write;
        }
    }

    /* Write argv pointers */
    for (int i = 0; i < n; i++) {
        uint32_t str_uva = strings_vaddr + (str_offsets[i] - scratch_top);
        WRITE_USER32(page_dir, argv_vaddr + (uint32_t)i * 4, str_uva);
    }
    WRITE_USER32(page_dir, argv_vaddr + (uint32_t)n * 4, 0); /* NULL terminator */
    WRITE_USER32(page_dir, argc_vaddr, (uint32_t)n);
    WRITE_USER32(page_dir, retaddr_vaddr, 0); /* dummy return address */

#undef WRITE_USER32

    *out_sp = retaddr_vaddr;

    DBGK("elf", "entry=0x%lx sp=0x%lx argc=%d\n", ehdr->e_entry, retaddr_vaddr, n);
    return ehdr->e_entry;
}
