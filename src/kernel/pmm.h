#pragma once

#include <stdint.h>

#define PAGE_SIZE 4096

void init_pmm(uint32_t mem_upper_kb);
uint32_t pmm_alloc_frame(void);       /* returns physical address, panics if OOM */
void pmm_free_frame(uint32_t addr);
void pmm_mark_used(uint32_t addr);    /* mark a frame as used (for identity mapping) */
