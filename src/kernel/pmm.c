/* Physical memory manager - bitmap-based frame allocator.
 *
 * Each bit in the bitmap represents one 4KB physical frame.
 * A set bit means the frame is allocated/reserved.
 *
 * The bitmap is statically allocated (128KB in BSS) to support
 * up to 4GB physical memory (1M frames).
 *
 * Two allocation zones:
 *   ZONE_LOW  0x00100000-0x01000000 (~15MB): for physically contiguous
 *             allocations (GUI back buffer, DMA). Scanned high->low to
 *             avoid colliding with the kernel image at the bottom.
 *   ZONE_ANY  0x01000000 and above: general purpose (task stacks,
 *             page tables, ELF segments). First-fit, low->high.
 *
 * Initialisation walks the Multiboot memory map: all frames start marked
 * used, then each MEMORY_TYPE_RAM region is freed, then kernel/heap/
 * reserved regions are re-marked used.
 */

#include <stdint.h>
#include <string.h>

#include "kernel/pmm.h"
#include "boot/multiboot.h"
#include "kernel/kernel.h"

#define MODULE "pmm: "

/* Support up to 4GB (1M frames). Bitmap costs 128KB in BSS. */
#define MAX_FRAMES (1024 * 1024)

/* Zone boundaries (frame indices) */
#define ZONE_LOW_START  (0x00100000 / PAGE_SIZE)   /* 1MB - skip low 1MB */
#define ZONE_LOW_END    (0x01000000 / PAGE_SIZE)   /* 16MB */
#define ZONE_ANY_START  (0x01000000 / PAGE_SIZE)   /* 16MB and above */

static uint32_t bitmap[MAX_FRAMES / 32];
static uint32_t nframes;   /* total frames under management */

/* CoW reference counts. Counts the number of owners of each frame.
 * 0 = free (no owners). 1 = solely owned. 2+ = shared (CoW).
 * pmm_alloc_frame_zone / pmm_alloc_contiguous initialise to 1 on allocation.
 * vmm_clone_page_dir increments for each shared frame.
 * vmm_destroy_page_dir decrements; frees the frame when it reaches 0. */
static uint16_t refcounts[MAX_FRAMES];

static inline void frame_set(uint32_t index)
{
    bitmap[index / 32] |= (1u << (index % 32));
}

static inline void frame_clear(uint32_t index)
{
    bitmap[index / 32] &= ~(1u << (index % 32));
}

static inline int frame_test(uint32_t index)
{
    return (bitmap[index / 32] >> (index % 32)) & 1;
}

uint32_t pmm_total_frames(void)
{
    return nframes;
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
    if (index < nframes) {
        frame_clear(index);
        refcounts[index] = 0;
    }
}

/* General allocator: first-fit scan from ZONE_ANY_START upward.
 * Falls back to ZONE_LOW if ZONE_ANY is exhausted. */
uint32_t pmm_alloc_frame(void)
{
    return pmm_alloc_frame_zone(PMM_ZONE_ANY);
}

uint32_t pmm_alloc_frame_zone(int zone)
{
    uint32_t start, end;

    if (zone == PMM_ZONE_LOW) {
        /* Low zone: scan high-to-low to preserve the low end for contiguous runs */
        start = ZONE_LOW_START;
        end   = (ZONE_LOW_END < nframes) ? ZONE_LOW_END : nframes;
        for (uint32_t i = end - 1; i >= start; i--) {
            if (!frame_test(i)) {
                frame_set(i);
                refcounts[i] = 1;
                return i * PAGE_SIZE;
            }
            if (i == start) break; /* avoid unsigned wraparound */
        }
        panic("pmm: out of ZONE_LOW frames");
    } else {
        /* Any zone: first-fit from ZONE_ANY_START, fallback to ZONE_LOW */
        start = ZONE_ANY_START;
        end   = nframes;
        for (uint32_t i = start; i < end; i++) {
            if (bitmap[i / 32] == 0xFFFFFFFF) {
                i = (i / 32 + 1) * 32 - 1; /* skip full word */
                continue;
            }
            if (!frame_test(i)) {
                frame_set(i);
                refcounts[i] = 1;
                return i * PAGE_SIZE;
            }
        }
        /* Fallback: try ZONE_LOW (single frame, not contiguous) */
        end = (ZONE_LOW_END < nframes) ? ZONE_LOW_END : nframes;
        for (uint32_t i = ZONE_LOW_START; i < end; i++) {
            if (!frame_test(i)) {
                frame_set(i);
                refcounts[i] = 1;
                return i * PAGE_SIZE;
            }
        }
        panic("pmm: out of physical frames");
    }
    return 0;
}

/* Allocate n physically contiguous frames from ZONE_LOW.
 * Returns base physical address on success, 0 on failure.
 * Scans low-to-high within ZONE_LOW for a run of n free frames. */
uint32_t pmm_alloc_contiguous(uint32_t n)
{
    if (n == 0) return 0;

    uint32_t end = (ZONE_LOW_END < nframes) ? ZONE_LOW_END : nframes;
    uint32_t run_start = 0;
    uint32_t run_len   = 0;

    for (uint32_t i = ZONE_LOW_START; i < end; i++) {
        if (!frame_test(i)) {
            if (run_len == 0) run_start = i;
            run_len++;
            if (run_len == n) {
                /* Found a run — mark all frames used with refcount 1 */
                for (uint32_t j = run_start; j < run_start + n; j++) {
                    frame_set(j);
                    refcounts[j] = 1;
                }
                return run_start * PAGE_SIZE;
            }
        } else {
            run_len = 0;
        }
    }
    return 0; /* not found */
}

void pmm_refcount_inc(uint32_t addr)
{
    uint32_t i = addr / PAGE_SIZE;
    if (i < nframes && refcounts[i] < 0xFFFF)
        refcounts[i]++;
}

uint32_t pmm_refcount_dec(uint32_t addr)
{
    uint32_t i = addr / PAGE_SIZE;
    if (i < nframes && refcounts[i] > 0)
        refcounts[i]--;
    return (i < nframes) ? refcounts[i] : 0;
}

uint32_t pmm_refcount_get(uint32_t addr)
{
    uint32_t i = addr / PAGE_SIZE;
    return (i < nframes) ? refcounts[i] : 0;
}

void init_pmm(struct multiboot_info *mb, uint32_t reserved_top)
{
    /* Determine the highest usable RAM address from the memory map */
    uint32_t highest = 0;
    if (mb->flags & (1 << 6)) {
        struct multiboot_memory_map *mmap =
            (struct multiboot_memory_map *)mb->mmap_addr;
        while ((unsigned long)mmap < mb->mmap_addr + mb->mmap_length) {
            if (mmap->type == MEMORY_TYPE_RAM && mmap->base_addr_high == 0) {
                uint32_t top = mmap->base_addr_low + mmap->length_low;
                if (top > highest)
                    highest = top;
            }
            mmap = (struct multiboot_memory_map *)
                   ((unsigned long)mmap + mmap->size + sizeof(mmap->size));
        }
    }

    /* Fallback to mem_upper if no mmap */
    if (highest == 0)
        highest = (mb->mem_upper + 1024) * 1024;

    nframes = highest / PAGE_SIZE;
    if (nframes > MAX_FRAMES)
        nframes = MAX_FRAMES;

    /* Start with all frames marked used — only free what the mmap says is RAM */
    memset(bitmap, 0xFF, sizeof(bitmap));

    if (mb->flags & (1 << 6)) {
        struct multiboot_memory_map *mmap =
            (struct multiboot_memory_map *)mb->mmap_addr;
        while ((unsigned long)mmap < mb->mmap_addr + mb->mmap_length) {
            if (mmap->type == MEMORY_TYPE_RAM && mmap->base_addr_high == 0) {
                uint32_t base = mmap->base_addr_low;
                uint32_t len  = mmap->length_low;
                /* Free page-aligned frames within this region */
                uint32_t frame_start = (base + PAGE_SIZE - 1) / PAGE_SIZE;
                uint32_t frame_end   = (base + len) / PAGE_SIZE;
                for (uint32_t i = frame_start; i < frame_end && i < nframes; i++)
                    frame_clear(i);
            }
            mmap = (struct multiboot_memory_map *)
                   ((unsigned long)mmap + mmap->size + sizeof(mmap->size));
        }
    }

    /* Always reserve frame 0 (null pointer trap) and low memory (0-1MB) */
    uint32_t low_frames = 0x100000 / PAGE_SIZE;
    for (uint32_t i = 0; i < low_frames && i < nframes; i++)
        frame_set(i);

    /* Reserve kernel image and any other pre-boot allocations up to reserved_top */
    if (reserved_top) {
        uint32_t top = (reserved_top + PAGE_SIZE - 1) / PAGE_SIZE;
        for (uint32_t i = low_frames; i < top && i < nframes; i++)
            frame_set(i);
    }

    /* Reserve multiboot module regions (initrd etc.) so PMM doesn't overwrite
     * them before they are parsed. */
    if (mb->flags & (1 << 3)) {
        struct multiboot_mod_entry *mods =
            (struct multiboot_mod_entry *)mb->mods_addr;
        for (uint32_t i = 0; i < mb->mods_count; i++) {
            uint32_t start = mods[i].mod_start / PAGE_SIZE;
            uint32_t end   = (mods[i].mod_end + PAGE_SIZE - 1) / PAGE_SIZE;
            for (uint32_t f = start; f < end && f < nframes; f++)
                frame_set(f);
        }
    }

    printk(MODULE "%ld MB usable, %ld frames (bitmap %ld KB)\n",
           highest / (1024 * 1024), nframes, (nframes / 32 * 4) / 1024);
}
