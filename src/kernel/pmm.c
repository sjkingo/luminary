/* Physical memory manager - bitmap-based frame allocator.
 *
 * Each bit in the bitmap represents one 4KB physical frame.
 * A set bit means the frame is allocated/reserved.
 * The bitmap is statically allocated to avoid chicken-and-egg
 * with the heap allocator.
 */

#include <stdint.h>
#include <string.h>

#include "kernel/kernel.h"
#include "kernel/pmm.h"

#define MODULE "pmm: "

/* Support up to 128MB (32768 frames). Costs 4KB in BSS. */
#define MAX_FRAMES 32768

static uint32_t frames[MAX_FRAMES / 32];
static uint32_t nframes;

static inline void frame_set(uint32_t index)
{
    frames[index / 32] |= (1 << (index % 32));
}

static inline void frame_clear(uint32_t index)
{
    frames[index / 32] &= ~(1 << (index % 32));
}

static inline int frame_test(uint32_t index)
{
    return frames[index / 32] & (1 << (index % 32));
}

void pmm_mark_used(uint32_t addr)
{
    uint32_t index = addr / PAGE_SIZE;
    if (index < nframes)
        frame_set(index);
}

void pmm_free_frame(uint32_t addr)
{
    uint32_t index = addr / PAGE_SIZE;
    if (index < nframes)
        frame_clear(index);
}

uint32_t pmm_alloc_frame(void)
{
    for (uint32_t i = 0; i < nframes / 32; i++) {
        if (frames[i] == 0xFFFFFFFF)
            continue;
        for (int bit = 0; bit < 32; bit++) {
            if (!(frames[i] & (1 << bit))) {
                uint32_t index = i * 32 + bit;
                if (index >= nframes)
                    break;
                frame_set(index);
                return index * PAGE_SIZE;
            }
        }
    }
    panic("pmm: out of physical frames");
}

void init_pmm(uint32_t mem_upper_kb)
{
    /* mem_upper is KB above 1MB. Total memory = mem_upper + 1024 KB */
    uint32_t total_kb = mem_upper_kb + 1024;
    uint32_t total_bytes = total_kb * 1024;

    nframes = total_bytes / PAGE_SIZE;
    if (nframes > MAX_FRAMES)
        nframes = MAX_FRAMES;

    /* Start with all frames free */
    memset(frames, 0, sizeof(frames));

    /* Reserve frame 0 (null page) */
    frame_set(0);

    printk(MODULE "%d MB physical memory, %d frames available\n",
           (unsigned)(total_kb / 1024), (unsigned)nframes);
}
