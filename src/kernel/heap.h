#pragma once

#include <stdint.h>

#define NUM_SLAB_CLASSES 8

#ifdef DEBUG
void *kmalloc_real(uint32_t, char const *, int, char const *);
void kfree_real(void *, char const *, int, char const *);
#define kmalloc(size) (kmalloc_real(size, __FILE__, __LINE__, __func__))
#define kfree(ptr)    (kfree_real(ptr, __FILE__, __LINE__, __func__))
#else
void *kmalloc(uint32_t);
void kfree(void *);
#endif

void init_kernel_heap(void *start_addr);
