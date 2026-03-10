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
    int32_t blocks_spanned; // only valid on the first block of an allocation
    void *ptr;              // only valid on the first block of an allocation
};

// This is a struct for future use so it's possible to add new elements
struct kernel_heap {
    struct kernel_heap_block blocks[KERNEL_HEAP_MAX_BLOCKS];
    int32_t free_blocks;
    void *data_area; // memory that heap will allocate in
};
static struct kernel_heap heap;

#ifdef DEBUG
static void dump_heap(void)
{
    printk(MODULE "dump_heap(): %ld blocks free |", heap.free_blocks);
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

#ifdef DEBUG
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
    if (required_blocks > heap.free_blocks)
        goto fail;

    // Iterate over the heap and find a contiguous run of free blocks
    int32_t run_start = -1;
    int32_t run_length = 0;
    for (int32_t i = 0; i < KERNEL_HEAP_MAX_BLOCKS; i++) {
        if (heap.blocks[i].allocated) {
            // Reset the run, this block breaks contiguity
            run_start = -1;
            run_length = 0;
            continue;
        }

        if (run_start == -1)
            run_start = i;
        run_length++;

        if (run_length == required_blocks) {
            // Found enough contiguous blocks, mark them allocated
            for (int32_t j = run_start; j <= i; j++) {
                heap.blocks[j].allocated = true;
                heap.blocks[j].blocks_spanned = 0;
                heap.blocks[j].ptr = NULL;
            }

            // Store metadata on the first block of the allocation
            void *ptr = heap.data_area + (run_start * KERNEL_HEAP_BLOCK_SIZE);
            heap.blocks[run_start].blocks_spanned = required_blocks;
            heap.blocks[run_start].ptr = ptr;

            heap.free_blocks -= required_blocks;

#ifdef DEBUG
            printk(MODULE "%s:%d(%s) kmalloc found %ld blocks (for %ld bytes) at 0x%lx\n",
                    calling_file, calling_line, calling_func, required_blocks,
                    size, (uint32_t)ptr);
#endif
            return ptr;
        }
    }

fail:
#ifdef DEBUG
    printk(MODULE "%s:%d(%s) kmalloc failed to allocate %ld bytes in %ld blocks\n",
            calling_file, calling_line, calling_func, size, required_blocks);
#endif
    return NULL;
}

#ifdef DEBUG
void kfree_real(void *ptr, char const *calling_file, int calling_line,
        char const *calling_func)
#else
void kfree(void *ptr)
#endif
{
    if (ptr == NULL)
        return;

    // Find the block that owns this pointer
    for (int32_t i = 0; i < KERNEL_HEAP_MAX_BLOCKS; i++) {
        struct kernel_heap_block *block = &heap.blocks[i];
        if (!block->allocated || block->ptr != ptr || block->blocks_spanned == 0)
            continue;

        // Found it, free this block and all blocks it spans
        int32_t count = block->blocks_spanned;

#ifdef DEBUG
        printk(MODULE "%s:%d(%s) kfree freeing %ld blocks at 0x%lx\n",
                calling_file, calling_line, calling_func, count, (uint32_t)ptr);
#endif

        for (int32_t j = i; j < i + count; j++) {
            heap.blocks[j].allocated = false;
            heap.blocks[j].blocks_spanned = 0;
            heap.blocks[j].ptr = NULL;
        }

        heap.free_blocks += count;
        return;
    }

    panic("kfree: pointer not found in heap");
}

void init_kernel_heap(void *start_addr)
{
    uint32_t heap_size = KERNEL_HEAP_MAX_BLOCKS * KERNEL_HEAP_BLOCK_SIZE;

    // clear the heap data area
    heap.data_area = start_addr;
    memset(heap.data_area, 0, heap_size);
    heap.free_blocks = KERNEL_HEAP_MAX_BLOCKS;

#ifdef DEBUG
    printk(MODULE "kernel heap 0x%lx -> 0x%lx (%ld KB available)\n", (uint32_t)heap.data_area,
        (uint32_t)heap.data_area + heap_size, heap_size/1024);
    printk(MODULE "heap consists of %d blocks of %d bytes each\n",
        KERNEL_HEAP_MAX_BLOCKS, KERNEL_HEAP_BLOCK_SIZE);
#endif
}
