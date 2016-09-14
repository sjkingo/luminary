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
#include "kernel/heap.h"

#define MODULE "heap: "

#define KERNEL_HEAP_BLOCK_SIZE 1024
#define KERNEL_HEAP_MAX_BLOCKS 1024

struct kernel_heap_block {
    bool allocated;
    int32_t blocks_spanned;
    void *ptr;
};

// This is a struct for future use so it's possible to add new elements
struct kernel_heap {
    struct kernel_heap_block blocks[KERNEL_HEAP_MAX_BLOCKS];
    int32_t free_blocks; // may be negative
    void *data_area; // memory that heap will allocate in
};
static struct kernel_heap heap;

#ifdef DEBUG_HEAP
static void dump_heap(void)
{
    printk(MODULE "dump_heap(): %d blocks free |", heap.free_blocks);
    for (int32_t i = 0; i < KERNEL_HEAP_MAX_BLOCKS; i++) {
        struct kernel_heap_block *block = &heap.blocks[i];
        if (block->allocated) {
            for (int32_t n = 0; n < block->blocks_spanned; n++)
                printk("X");
            if (block->blocks_spanned > 0)
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
    int32_t required_blocks = size / KERNEL_HEAP_BLOCK_SIZE;
    if ((size % KERNEL_HEAP_BLOCK_SIZE) != 0)
        required_blocks++;

    // Shortcut to avoid looping if the allocation requires too many blocks
    if (required_blocks > KERNEL_HEAP_MAX_BLOCKS)
        goto fail;

    // Iterate over the heap and find a big enough gap to allocate
    struct kernel_heap_block *picked_block = NULL;
    uint32_t i;
    for (i = 0; i < KERNEL_HEAP_MAX_BLOCKS; i++) {
        struct kernel_heap_block *this_block = &heap.blocks[i];
        if (this_block->allocated)
            continue;

        if (picked_block == NULL)
            picked_block = this_block;
        picked_block->blocks_spanned++;

        // if we have found a suitable free space, mark it and all spanned
        // blocks as allocated and exit. We need to do this backwards since the
        // picked_block is the *last* block in the chain that fits
        if (picked_block != NULL && picked_block->blocks_spanned == required_blocks) {
            for (int32_t offset = 0; offset < required_blocks; offset++) {
                heap.blocks[i - offset].allocated = true;
            }
            break;
        }
    }

    if (picked_block == NULL || !picked_block->allocated)
        goto fail;

    picked_block->ptr = heap.data_area + (i * KERNEL_HEAP_BLOCK_SIZE);
    heap.free_blocks -= required_blocks;
    if (heap.free_blocks < 0)
        panic("free_blocks < 0: bug in allocator?");

#ifdef DEBUG_HEAP
    printk(MODULE "%s:%d(%s) kmalloc found %d blocks (for %d bytes) to allocate at 0x%x\n",
            calling_file, calling_line, calling_func, picked_block->blocks_spanned,
            size, (uint32_t)picked_block->ptr);
#endif

    return picked_block->ptr;

fail:
#ifdef DEBUG_HEAP
    printk(MODULE "%s:%d(%s) kmalloc failed to allocate %d bytes in %d blocks\n",
            calling_file, calling_line, calling_func, size, required_blocks);
#endif
    // if we are failing, make sure there isn't a bug in this allocator
    if (required_blocks <= heap.free_blocks)
        panic("bug in allocator?");
    return NULL;
}

void init_kernel_heap(void *start_addr)
{
    uint32_t heap_size = KERNEL_HEAP_MAX_BLOCKS * KERNEL_HEAP_BLOCK_SIZE;

    // clear the heap data area
    heap.data_area = start_addr;
    memset(heap.data_area, 0, heap_size);
    heap.free_blocks = KERNEL_HEAP_MAX_BLOCKS;

#ifdef DEBUG_HEAP
    printk(MODULE "kernel heap 0x%x -> 0x%x (%d KB available)\n", (uint32_t)heap.data_area,
        (uint32_t)heap.data_area + heap_size, heap_size/1024);
    printk(MODULE "heap consists of %d blocks of %d bytes each\n",
        KERNEL_HEAP_MAX_BLOCKS, KERNEL_HEAP_BLOCK_SIZE);
#endif
}
