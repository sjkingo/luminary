/* Kernel heap implementation. The kernel's memory manager
 * is a fixed-size block allocator, which maintains a pool of
 * fixed-size blocks for use when calling kmalloc().
 *
 * Since there is no virtual address space in the kernel, the
 * allocator makes use of a contiguous chunk of memory and a
 * compiled-in maximum heap size.
 *
 * It is designed for efficient, constant-time access.
 *
 * The implementation details are described below:
 */

#include <stdbool.h>
#include <stdint.h>

#include "kernel/kernel.h"

#define MODULE "heap: "

#define KERNEL_HEAP_BLOCK_SIZE 1024
#define KERNEL_HEAP_MAX_BLOCKS 512

struct kernel_heap_block {
    bool allocated;
    uint32_t blocks_spanned;
    void *ptr;
};

// This is a struct for future use so it's possible to add new elements
struct kernel_heap {
    struct kernel_heap_block blocks[KERNEL_HEAP_MAX_BLOCKS];
};
static struct kernel_heap *heap;

#define KERNEL_HEAP_SIZE (KERNEL_HEAP_BLOCK_SIZE * KERNEL_HEAP_MAX_BLOCKS)

#ifdef DEBUG_HEAP
static void dump_heap(void)
{
    printk(MODULE "|");
    for (uint32_t i = 0; i < KERNEL_HEAP_MAX_BLOCKS; i++) {
        struct kernel_heap_block *block = &heap->blocks[i];
        if (block->allocated) {
            for (uint32_t n = 0; n < block->blocks_spanned; n++)
                printk("X");
            printk("|");
        } else {
            printk("-|");
        }
    }
    printk("\n");
}
#endif

#ifdef DEBUG_HEAP
void *kmalloc_real(uint32_t size, char const *calling_file, int calling_line,
        char const *calling_func)
#else
void *kmalloc(uint32_t size)
#endif
{
    // Determine how many blocks this allocation will span
    uint32_t required_blocks = size / KERNEL_HEAP_BLOCK_SIZE;
    if ((size % KERNEL_HEAP_BLOCK_SIZE) != 0)
        required_blocks++;

    // Shortcut to avoid looping if the allocation requires too many blocks
    if (required_blocks > KERNEL_HEAP_MAX_BLOCKS)
        goto fail;

#ifdef DEBUG_HEAP
    printk(MODULE "%s:%d(%s) called kmalloc(%d), finding %d blocks\n",
            calling_file, calling_line, calling_func, size, required_blocks);
#endif

    // Iterate over the heap and find a big enough gap to allocate
    struct kernel_heap_block *picked_block = NULL;
    for (uint32_t i = 0; i <= KERNEL_HEAP_MAX_BLOCKS; i++) {
        struct kernel_heap_block *this_block = &heap->blocks[i];
        if (this_block->allocated)
            continue;

        // if we have found a suitable free space, mark as allocated and exit
        if (picked_block != NULL && picked_block->blocks_spanned == required_blocks) {
            picked_block->allocated = true;
            break;
        }

        if (picked_block == NULL)
            picked_block = this_block;
        picked_block->blocks_spanned++;
    }

    if (picked_block == NULL || !picked_block->allocated)
        goto fail;

#ifdef DEBUG_HEAP
    printk(MODULE "%s:%d(%s) kmalloc found %d blocks to allocate\n",
            calling_file, calling_line, calling_func, picked_block->blocks_spanned);
    dump_heap();
#endif
    return picked_block;

fail:
#ifdef DEBUG_HEAP
    printk(MODULE "%s:%d(%s) kmalloc failed to allocate %d bytes in %d blocks\n",
            calling_file, calling_line, calling_func, size, required_blocks);
#endif
    return NULL;
}

void init_kernel_heap(void *start_addr)
{
    heap = start_addr;
    memset(heap, 0, KERNEL_HEAP_SIZE);

#ifdef DEBUG_HEAP
    printk(MODULE "kernel heap 0x%x -> 0x%x (%d KB available)\n", (uint32_t)heap,
        (uint32_t)heap + sizeof(*heap) + KERNEL_HEAP_SIZE, KERNEL_HEAP_SIZE/1024);
    printk(MODULE "heap has %d blocks of %d bytes each\n",
        KERNEL_HEAP_MAX_BLOCKS, KERNEL_HEAP_BLOCK_SIZE);
    dump_heap();
#endif
}
