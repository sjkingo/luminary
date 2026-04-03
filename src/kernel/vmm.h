#pragma once

#include <stdint.h>

/* Page table entry flags */
#define PTE_PRESENT   0x001
#define PTE_WRITE     0x002
#define PTE_USER      0x004

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

/* Clone a page directory: deep-copy all user-space mappings (PDE indices
 * covering USER_SPACE_START..USER_SPACE_END) into a new page directory.
 * Each mapped user frame is copied to a fresh physical frame.
 * Kernel PDEs are shared by reference as in vmm_create_page_dir().
 * Returns physical address of the new page directory. */
uint32_t vmm_clone_page_dir(uint32_t src_dir_phys);
