#pragma once

#include <stdint.h>

#define PAGE_SIZE 4096

struct multiboot_info;

/* Allocation zones */
#define PMM_ZONE_LOW  0   /* low physical memory: contiguous, identity-mapped */
#define PMM_ZONE_ANY  1   /* general purpose: task stacks, page tables, ELF */

void     init_pmm(struct multiboot_info *mb, uint32_t reserved_top);
uint32_t pmm_alloc_frame(void);                  /* general alloc, ZONE_ANY */
uint32_t pmm_alloc_frame_zone(int zone);         /* alloc from specific zone */
uint32_t pmm_alloc_contiguous(uint32_t n);       /* n contiguous frames from ZONE_LOW; 0 on fail */
void     pmm_free_frame(uint32_t addr);
void     pmm_mark_used(uint32_t addr);           /* mark a frame as used */
uint32_t pmm_total_frames(void);                 /* total managed frames */
