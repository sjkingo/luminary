/* Slab allocator.
 *
 * Size classes: 32, 64, 128, 256, 512, 1024, 2048, 4096 bytes.
 * Each class maintains a list of 4KB PMM pages. Within each page, slots
 * are tracked by a 128-bit bitmap (4 × uint32_t), where bit=0 means free
 * and bit=1 means allocated.
 *
 * For allocations > 4096 bytes, falls back to vmm_alloc_pages tracked in
 * a small overflow table. Freed overflow blocks are held in a large-object
 * cache so repeated alloc/free of the same size avoids PTE churn.
 *
 * Thread safety: this kernel is single-core and kmalloc/kfree are never
 * called from IRQ context. Callers that need atomicity (e.g. task creation)
 * disable interrupts themselves before calling kmalloc.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "kernel/kernel.h"
#include "kernel/heap.h"
#include "kernel/pmm.h"
#include "kernel/vmm.h"

/* ── size classes ────────────────────────────────────────────────────────── */

static const uint32_t slab_sizes[NUM_SLAB_CLASSES] = {
    32, 64, 128, 256, 512, 1024, 2048, 4096
};

/* ── per-page metadata ───────────────────────────────────────────────────── */

#define SLAB_BITMAP_WORDS 4     /* 4 × 32 = 128 bits; enough for 4096/32 = 128 slots */

struct slab_page {
    uint32_t phys;              /* physical (= virtual, identity-mapped) address of page */
    uint32_t n_slots;           /* total slots in this page */
    uint32_t n_free;            /* number of free slots remaining */
    uint32_t bitmap[SLAB_BITMAP_WORDS]; /* 0 = free, 1 = allocated */
};

/* ── per-class state ─────────────────────────────────────────────────────── */

#define MAX_SLABS_PER_CLASS 64

struct slab_class {
    uint32_t         obj_size;
    uint32_t         n_pages;
    struct slab_page pages[MAX_SLABS_PER_CLASS];
};

static struct slab_class classes[NUM_SLAB_CLASSES];

/* ── overflow table for allocations > 4096 bytes ────────────────────────── */

#define MAX_OVERFLOW 128

struct overflow_entry {
    uint32_t virt;      /* base virtual address (from vmm_alloc_pages) */
    uint32_t nframes;   /* number of frames allocated */
};

static struct overflow_entry overflow_table[MAX_OVERFLOW];

/* Large-object cache: instead of unmapping on kfree, keep the virtual range
 * alive keyed by nframes.  The next kmalloc of the same size returns it
 * directly — zero PTE writes, zero TLB flushes. */
#define LARGE_CACHE_MAX 8

struct large_cache_entry {
    uint32_t virt;
    uint32_t nframes;
};

static struct large_cache_entry large_cache[LARGE_CACHE_MAX];
static uint32_t large_cache_count = 0;

/* ── helpers ─────────────────────────────────────────────────────────────── */

/* Find the smallest size class that fits 'size' bytes.
 * Returns NUM_SLAB_CLASSES if size > 4096 (overflow path). */
static int find_class(uint32_t size)
{
    for (int i = 0; i < NUM_SLAB_CLASSES; i++) {
        if (size <= slab_sizes[i])
            return i;
    }
    return NUM_SLAB_CLASSES;
}

/* Allocate a new slab page for the given class. Returns 1 on success. */
static int slab_grow(struct slab_class *cls)
{
    if (cls->n_pages >= MAX_SLABS_PER_CLASS)
        return 0;

    /* vmm_alloc_pages maps the frame into the kernel virtual range (0xC0000000+)
     * so it never overlaps with user space virtual addresses. */
    void *virt = vmm_alloc_pages(1);
    if (!virt) return 0;
    memset(virt, 0, PAGE_SIZE);

    struct slab_page *sp = &cls->pages[cls->n_pages++];
    sp->phys    = (uint32_t)virt;   /* field stores virtual addr; name kept for compat */
    sp->n_slots = PAGE_SIZE / cls->obj_size;
    sp->n_free  = sp->n_slots;
    memset(sp->bitmap, 0, sizeof(sp->bitmap)); /* all free */
    return 1;
}

/* Allocate one slot from a slab page. Returns NULL if page is full. */
static void *slab_page_alloc(struct slab_page *sp, uint32_t obj_size)
{
    for (int w = 0; w < SLAB_BITMAP_WORDS; w++) {
        if (sp->bitmap[w] == 0xFFFFFFFFu)
            continue;
        int bit = __builtin_ctz(~sp->bitmap[w]); /* index of first 0-bit */
        uint32_t slot = (uint32_t)(w * 32 + bit);
        if (slot >= sp->n_slots)
            continue;
        sp->bitmap[w] |= (1u << bit);
        sp->n_free--;
        return (void *)(sp->phys + slot * obj_size);
    }
    return NULL;
}

/* ── public API ──────────────────────────────────────────────────────────── */

#ifdef DEBUG
void *kmalloc_real(uint32_t size, char const *file, int line, char const *func)
#else
void *kmalloc(uint32_t size)
#endif
{
    if (size == 0) return NULL;

    int ci = find_class(size);

    if (ci == NUM_SLAB_CLASSES) {
        /* Overflow: allocate contiguous PMM frames */
        uint32_t nframes = (size + PAGE_SIZE - 1) / PAGE_SIZE;

        /* Find a free overflow slot */
        int slot = -1;
        for (int i = 0; i < MAX_OVERFLOW; i++) {
            if (overflow_table[i].virt == 0) { slot = i; break; }
        }
        if (slot < 0) {
            DBGK("%s:%d(%s) overflow table full (size=%ld)\n",
                 path_basename(file), line, func, size);
            return NULL;
        }

        /* Check large-object cache before calling vmm_alloc_pages. */
        uint32_t virt = 0;
        for (uint32_t i = 0; i < large_cache_count; i++) {
            if (large_cache[i].nframes == nframes) {
                virt = large_cache[i].virt;
                large_cache[i] = large_cache[--large_cache_count];
                break;
            }
        }
        if (!virt) {
            void *p = vmm_alloc_pages(nframes);
            if (!p) {
                DBGK("%s:%d(%s) overflow alloc failed (size=%ld)\n",
                     path_basename(file), line, func, size);
                return NULL;
            }
            virt = (uint32_t)p;
        }

        overflow_table[slot].virt    = virt;
        overflow_table[slot].nframes = nframes;
        return (void *)virt;
    }

    struct slab_class *cls = &classes[ci];

    /* Try existing pages first */
    for (uint32_t i = 0; i < cls->n_pages; i++) {
        if (cls->pages[i].n_free == 0) continue;
        void *ptr = slab_page_alloc(&cls->pages[i], cls->obj_size);
        if (ptr)
            return ptr;
    }

    /* No free slot — grow the class */
    if (!slab_grow(cls)) {
        DBGK("%s:%d(%s) slab[%ld] grow failed (size=%ld)\n",
             path_basename(file), line, func, cls->obj_size, size);
        return NULL;
    }

    /* Allocate from the newly added page */
    return slab_page_alloc(&cls->pages[cls->n_pages - 1], cls->obj_size);
}

#ifdef DEBUG
void kfree_real(void *ptr, char const *file, int line, char const *func)
#else
void kfree(void *ptr)
#endif
{
    if (!ptr) return;

    uint32_t addr = (uint32_t)ptr;

    /* Check overflow table first */
    for (int i = 0; i < MAX_OVERFLOW; i++) {
        if (overflow_table[i].virt == addr) {
            uint32_t nframes = overflow_table[i].nframes;
            overflow_table[i].virt    = 0;
            overflow_table[i].nframes = 0;

            /* Push to large-object cache instead of unmapping. */
            if (large_cache_count < LARGE_CACHE_MAX) {
                large_cache[large_cache_count].virt    = addr;
                large_cache[large_cache_count].nframes = nframes;
                large_cache_count++;
            } else {
                /* Cache full — evict oldest (index 0) and free it. */
                vmm_free_pages((void *)large_cache[0].virt, large_cache[0].nframes);
                large_cache[0] = large_cache[LARGE_CACHE_MAX - 1];
                large_cache[LARGE_CACHE_MAX - 1].virt    = addr;
                large_cache[LARGE_CACHE_MAX - 1].nframes = nframes;
            }
            return;
        }
    }

    /* Search slab pages */
    for (int ci = 0; ci < NUM_SLAB_CLASSES; ci++) {
        struct slab_class *cls = &classes[ci];
        for (uint32_t pi = 0; pi < cls->n_pages; pi++) {
            struct slab_page *sp = &cls->pages[pi];
            if (addr < sp->phys || addr >= sp->phys + PAGE_SIZE)
                continue;
            /* ptr is within this slab page */
            uint32_t slot = (addr - sp->phys) / cls->obj_size;
            /* Validate alignment */
            if (sp->phys + slot * cls->obj_size != addr) {
                DBGK("%s:%d(%s) misaligned pointer 0x%lx\n",
                     path_basename(file), line, func, addr);
                panic("kfree: misaligned pointer");
            }
            uint32_t w   = slot / 32;
            uint32_t bit = slot % 32;
            if (!(sp->bitmap[w] & (1u << bit))) {
                DBGK("%s:%d(%s) double free at 0x%lx\n",
                     path_basename(file), line, func, addr);
                panic("kfree: double free");
            }
            sp->bitmap[w] &= ~(1u << bit);
            sp->n_free++;
            return;
        }
    }

    panic("kfree: pointer not found in any slab");
}

void init_kernel_heap(void *start_addr)
{
    (void)start_addr; /* ignored — we use PMM directly now */

    memset(classes, 0, sizeof(classes));
    memset(overflow_table, 0, sizeof(overflow_table));

    for (int i = 0; i < NUM_SLAB_CLASSES; i++)
        classes[i].obj_size = slab_sizes[i];

    printk("heap: slab allocator ready (%d classes: 32..4096 bytes)\n",
           NUM_SLAB_CLASSES);
}
