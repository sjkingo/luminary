/* Virtual memory manager - page directory and page table management.
 *
 * Sets up identity-mapped paging so that virtual addresses equal
 * physical addresses. This preserves all existing pointer values
 * (VGA buffer, VBE framebuffer, kernel code, heap, etc.) while
 * enabling memory protection via page faults.
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

void vmm_map_page(uint32_t virt, uint32_t phys, uint32_t flags)
{
    uint32_t pd_index = virt >> 22;
    uint32_t pt_index = (virt >> 12) & 0x3FF;

    /* Get or create page table */
    if (!(page_directory[pd_index] & PTE_PRESENT)) {
        uint32_t pt_frame = pmm_alloc_frame();
        memset((void *)pt_frame, 0, PAGE_SIZE);
        page_directory[pd_index] = pt_frame | PTE_PRESENT | PTE_WRITE;
    }

    uint32_t *page_table = (uint32_t *)(page_directory[pd_index] & 0xFFFFF000);
    page_table[pt_index] = (phys & 0xFFFFF000) | (flags & 0xFFF);
}

void vmm_unmap_page(uint32_t virt)
{
    uint32_t pd_index = virt >> 22;
    uint32_t pt_index = (virt >> 12) & 0x3FF;

    if (!(page_directory[pd_index] & PTE_PRESENT))
        return;

    uint32_t *page_table = (uint32_t *)(page_directory[pd_index] & 0xFFFFF000);
    page_table[pt_index] = 0;

    /* Flush TLB for this address */
    asm volatile("invlpg (%0)" : : "r"(virt) : "memory");
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

    /* Identity-map the heap region explicitly in case it extends past 4MB.
     * Heap starts at kernel_end + 1MB. kernel_end is around 0x105000,
     * so heap is around 0x205000 to 0x305000. Already covered by the
     * 4MB mapping above, but be safe and extend to 8MB.
     */
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
            printk(MODULE "mapping VBE framebuffer 0x%x - 0x%x (%d KB)\n",
                   (unsigned)fb_start, (unsigned)(fb_start + fb_size),
                   (unsigned)(fb_size / 1024));
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

    printk(MODULE "page directory at 0x%x\n", (unsigned)(uint32_t)page_directory);

    /* Load page directory into CR3 */
    asm volatile("mov %0, %%cr3" : : "r"(page_directory));

    /* Enable paging (set CR0.PG, bit 31) */
    uint32_t cr0;
    asm volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= 0x80000000;
    asm volatile("mov %0, %%cr0" : : "r"(cr0));

    printk(MODULE "paging enabled\n");
}
