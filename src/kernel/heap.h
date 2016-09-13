#pragma once

#ifdef DEBUG_HEAP
void *kmalloc_real(uint32_t, char const *, int, char const *);
#define kmalloc(size) (kmalloc_real(size, __FILE__, __LINE__, __func__))
#else
void *kmalloc(uint32_t);
#endif

void init_kernel_heap(void *start_addr);
