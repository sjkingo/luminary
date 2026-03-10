/* Virtual memory manager - page directory and page table management.
 *
 * Sets up identity-mapped paging so that virtual addresses equal
 * physical addresses. This preserves all existing pointer values
 * (VGA buffer, VBE framebuffer, kernel code, heap, etc.) while
 * enabling memory protection via page faults.
 *
 * Per-task page directories share kernel page tables by reference.
 * User-space page tables are private to each task.
 *
 * Page directory and page tables use raw uint32_t entries with
 * bitwise flag manipulation - no bitfield structs.
 */

#include <stdint.h>
#include <string.h>

#include "boot/multiboot.h"
#include "drivers/vbe.h"
#include "kernel/kernel.h"
#include "kernel/pmm.h"
#include "kernel/vmm.h"

#define MODULE "vmm: "

/* Page directory - statically allocated, page-aligned */
static uint32_t page_directory[1024] __attribute__((aligned(4096)));

void vmm_map_page_in(uint32_t dir_phys, uint32_t virt, uint32_t phys, uint32_t flags)
{
    uint32_t *dir = (uint32_t *)dir_phys;
    uint32_t pd_index = virt >> 22;
    uint32_t pt_index = (virt >> 12) & 0x3FF;

    /* Get or create page table */
    if (!(dir[pd_index] & PTE_PRESENT)) {
        uint32_t pt_frame = pmm_alloc_frame();
        /* For non-kernel directories, identity-map the page table frame
         * in the kernel directory so it's accessible. For the kernel
         * directory itself, page tables for 0-128MB are pre-allocated
         * during init_vmm. */
        if (dir_phys != (uint32_t)page_directory)
            vmm_map_page(pt_frame, pt_frame, PTE_PRESENT | PTE_WRITE);
        memset((void *)pt_frame, 0, PAGE_SIZE);
        dir[pd_index] = pt_frame | PTE_PRESENT | PTE_WRITE | (flags & PTE_USER);
    } else if ((flags & PTE_USER) && !(dir[pd_index] & PTE_USER)) {
        /* PDE exists but lacks user access - add it. Both the PDE and
         * PTE must have PTE_USER for ring 3 access to work. */
        dir[pd_index] |= PTE_USER;
    }

    uint32_t *page_table = (uint32_t *)(dir[pd_index] & 0xFFFFF000);
    page_table[pt_index] = (phys & 0xFFFFF000) | (flags & 0xFFF);
}

void vmm_unmap_page_in(uint32_t dir_phys, uint32_t virt)
{
    uint32_t *dir = (uint32_t *)dir_phys;
    uint32_t pd_index = virt >> 22;
    uint32_t pt_index = (virt >> 12) & 0x3FF;

    if (!(dir[pd_index] & PTE_PRESENT))
        return;

    uint32_t *page_table = (uint32_t *)(dir[pd_index] & 0xFFFFF000);
    page_table[pt_index] = 0;

    /* Flush TLB if this is the currently loaded page directory */
    uint32_t cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    if (cr3 == dir_phys)
        asm volatile("invlpg (%0)" : : "r"(virt) : "memory");
}

void vmm_map_page(uint32_t virt, uint32_t phys, uint32_t flags)
{
    vmm_map_page_in((uint32_t)page_directory, virt, phys, flags);
}

void vmm_unmap_page(uint32_t virt)
{
    vmm_unmap_page_in((uint32_t)page_directory, virt);
}

uint32_t vmm_get_phys(uint32_t virt)
{
    uint32_t pd_index = virt >> 22;
    uint32_t pt_index = (virt >> 12) & 0x3FF;

    if (!(page_directory[pd_index] & PTE_PRESENT))
        return 0;

    uint32_t *page_table = (uint32_t *)(page_directory[pd_index] & 0xFFFFF000);
    if (!(page_table[pt_index] & PTE_PRESENT))
        return 0;

    return (page_table[pt_index] & 0xFFFFF000) | (virt & 0xFFF);
}

uint32_t vmm_get_kernel_page_dir(void)
{
    return (uint32_t)page_directory;
}

uint32_t vmm_create_page_dir(void)
{
    uint32_t dir_frame = pmm_alloc_frame();

    /* Identity-map the new frame so we can write to it. This is needed
     * because pmm_alloc_frame() may return addresses beyond the
     * initial 8MB identity-mapped region. */
    vmm_map_page(dir_frame, dir_frame, PTE_PRESENT | PTE_WRITE);

    uint32_t *dir = (uint32_t *)dir_frame;

    /* Copy all PDEs from the kernel page directory. This shares
     * kernel page tables by reference - any kernel mapping change
     * is automatically visible in all task directories. */
    memcpy(dir, page_directory, PAGE_SIZE);

    return dir_frame;
}

void vmm_destroy_page_dir(uint32_t dir_phys)
{
    if (dir_phys == (uint32_t)page_directory)
        panic("vmm_destroy_page_dir: cannot destroy kernel page directory");

    uint32_t *dir = (uint32_t *)dir_phys;

    for (int i = 0; i < 1024; i++) {
        if (!(dir[i] & PTE_PRESENT))
            continue;

        /* Skip entries that match the kernel template (shared kernel tables) */
        if (dir[i] == page_directory[i])
            continue;

        /* This is a user page table - free mapped frames and the table itself */
        uint32_t *pt = (uint32_t *)(dir[i] & 0xFFFFF000);
        for (int j = 0; j < 1024; j++) {
            if (pt[j] & PTE_PRESENT)
                pmm_free_frame(pt[j] & 0xFFFFF000);
        }
        pmm_free_frame(dir[i] & 0xFFFFF000);
    }

    pmm_free_frame(dir_phys);
}

void vmm_switch_page_dir(uint32_t dir_phys)
{
    asm volatile("mov %0, %%cr3" : : "r"(dir_phys) : "memory");
}

/* Identity-map a range of physical addresses (page-aligned) */
static void identity_map_range(uint32_t start, uint32_t end, uint32_t flags)
{
    /* Align start down, end up to page boundaries */
    start &= 0xFFFFF000;
    end = (end + PAGE_SIZE - 1) & 0xFFFFF000;

    for (uint32_t addr = start; addr < end; addr += PAGE_SIZE) {
        vmm_map_page(addr, addr, flags);
        pmm_mark_used(addr);
    }
}

void init_vmm(void)
{
    /* Zero the page directory */
    memset(page_directory, 0, sizeof(page_directory));

    /* Mark the page directory itself as used */
    pmm_mark_used((uint32_t)page_directory);

    /* Identity-map the first 4MB (covers low memory, kernel, stack, VGA).
     * The kernel loads at 1MB, heap starts at kernel_end + 1MB, so 4MB
     * covers all of: BIOS data, kernel code/data/bss/stack, and most
     * of the heap.
     */
    identity_map_range(0x00000000, 0x00400000, PTE_PRESENT | PTE_WRITE);

    /* Identity-map 4-8MB. Covers the heap region (starts around 0x205000). */
    identity_map_range(0x00400000, 0x00800000, PTE_PRESENT | PTE_WRITE);

    /* Identity-map the VBE framebuffer region if available.
     * Typically at 0xFD000000, roughly 3-4MB in size.
     */
    if (mb_info->vbe_mode_info) {
        struct vbe_mode_info_struct *vbe = mb_info->vbe_mode_info;
        if (vbe->framebuffer) {
            uint32_t fb_start = vbe->framebuffer;
            /* pitch * height gives the actual framebuffer size */
            uint32_t fb_size = (uint32_t)vbe->pitch * vbe->height;
            if (fb_size == 0)
                fb_size = 4 * 1024 * 1024; /* fallback: 4MB */
            printk(MODULE "mapping VBE framebuffer 0x%lx - 0x%lx (%ld KB)\n",
                   fb_start, fb_start + fb_size, fb_size / 1024);
            identity_map_range(fb_start, fb_start + fb_size,
                               PTE_PRESENT | PTE_WRITE);
        }
    }

    /* Also map the page directory and page table frames themselves.
     * The page directory is in BSS (already covered by 0-8MB mapping).
     * Page tables were allocated from pmm_alloc_frame() which returns
     * frames in the 8MB+ range - those are identity-mapped because
     * we access page tables at their physical address, and we mapped
     * them before paging was enabled.
     */

    /* Pre-allocate page tables for the first 128MB of physical memory.
     * This ensures vmm_map_page() can identity-map any frame returned
     * by pmm_alloc_frame() without needing to allocate a new page table
     * (which would cause a chicken-and-egg problem after paging is enabled).
     * Cost: 32 frames (128KB) for page tables covering 128MB.
     * Each page table frame is also identity-mapped so it remains
     * accessible after paging is enabled. */
    for (uint32_t addr = 0; addr < 128 * 1024 * 1024; addr += 4 * 1024 * 1024) {
        uint32_t pd_index = addr >> 22;
        if (!(page_directory[pd_index] & PTE_PRESENT)) {
            uint32_t pt_frame = pmm_alloc_frame();
            memset((void *)pt_frame, 0, PAGE_SIZE);
            page_directory[pd_index] = pt_frame | PTE_PRESENT | PTE_WRITE;
            /* Identity-map the page table frame itself so we can
             * write PTEs into it after paging is enabled */
            vmm_map_page(pt_frame, pt_frame, PTE_PRESENT | PTE_WRITE);
        }
    }

    printk(MODULE "page directory at 0x%lx\n", (uint32_t)page_directory);

    /* Load page directory into CR3 */
    asm volatile("mov %0, %%cr3" : : "r"(page_directory));

    /* Enable paging (set CR0.PG, bit 31) */
    uint32_t cr0;
    asm volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= 0x80000000;
    asm volatile("mov %0, %%cr0" : : "r"(cr0));

    printk(MODULE "paging enabled\n");
}
