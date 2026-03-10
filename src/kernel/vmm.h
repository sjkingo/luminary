#pragma once

#include <stdint.h>

/* Page table entry flags */
#define PTE_PRESENT   0x001
#define PTE_WRITE     0x002
#define PTE_USER      0x004

void init_vmm(void);
void vmm_map_page(uint32_t virt, uint32_t phys, uint32_t flags);
void vmm_unmap_page(uint32_t virt);
uint32_t vmm_get_phys(uint32_t virt);   /* translate virtual to physical */
