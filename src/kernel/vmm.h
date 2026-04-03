#pragma once

#include <stdint.h>

/* Page table entry flags */
#define PTE_PRESENT   0x001
#define PTE_WRITE     0x002
#define PTE_USER      0x004
#define PTE_COW       0x200   /* OS-reserved bit 9: copy-on-write page */

/* Address space layout */
#define USER_SPACE_START    0x01000000
#define USER_SPACE_END      0xC0000000
#define USER_STACK_TOP      0xBFFFF000

void init_vmm(void);
void vmm_map_page(uint32_t virt, uint32_t phys, uint32_t flags);
void vmm_unmap_page(uint32_t virt);
uint32_t vmm_get_phys(uint32_t virt);   /* translate virtual to physical */

/* Page directory management */
uint32_t vmm_get_kernel_page_dir(void);
uint32_t vmm_create_page_dir(void);
void vmm_destroy_page_dir(uint32_t dir_phys);
void vmm_switch_page_dir(uint32_t dir_phys);

/* Map/unmap pages in a specific page directory */
void vmm_map_page_in(uint32_t dir_phys, uint32_t virt, uint32_t phys, uint32_t flags);
void vmm_unmap_page_in(uint32_t dir_phys, uint32_t virt);

/* Kernel virtual page allocator: maps n physical frames to a fresh contiguous
 * virtual range above 0xC0000000. Physical frames need not be contiguous. */
void *vmm_alloc_pages(uint32_t n);
void  vmm_free_pages(void *virt_base, uint32_t n);

/* Temporarily map/unmap a physical frame into kernel virtual address space.
 * vmm_kmap does not allocate a physical frame; vmm_kunmap does not free it. */
void *vmm_kmap(uint32_t phys);
void  vmm_kunmap(void *virt);

/* Clone a page directory using copy-on-write: shared frames are marked
 * read-only + PTE_COW in both parent and child. Data is only copied on
 * the first write to a shared page (handled by vmm_cow_fault).
 * Returns physical address of the new page directory. */
uint32_t vmm_clone_page_dir(uint32_t src_dir_phys);

/* Handle a CoW page fault: called from the page fault handler when a write
 * fault occurs on a PTE_COW page. dir_phys is the faulting task's page
 * directory; fault_addr is the faulting virtual address.
 * Returns 1 if the fault was a valid CoW fault and was resolved, 0 otherwise. */
int vmm_cow_fault(uint32_t dir_phys, uint32_t fault_addr);
