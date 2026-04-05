/* Virtual memory manager - page directory and page table management.
 *
 * Sets up identity-mapped paging so that virtual addresses equal
 * physical addresses. This preserves all existing pointer values
 * (VGA buffer, VBE framebuffer, kernel code, heap, etc.) while
 * enabling memory protection via page faults.
 *
 * Per-task page directories share kernel page tables by reference.
 * User-space page tables are private to each task.
 *
 * Page directory and page tables use raw uint32_t entries with
 * bitwise flag manipulation - no bitfield structs.
 */

#include <stdint.h>
#include <string.h>

#include "boot/multiboot.h"
#include "drivers/vbe.h"
#include "kernel/kernel.h"
#include "kernel/pmm.h"
#include "kernel/vmm.h"

/* Flush TLB for a single page in the currently loaded page directory */
static inline void tlb_flush(uint32_t virt)
{
    asm volatile("invlpg (%0)" : : "r"(virt) : "memory");
}

#define MODULE "vmm: "

/* Page directory - statically allocated, page-aligned */
static uint32_t page_directory[1024] __attribute__((aligned(4096)));

void vmm_map_page_in(uint32_t dir_phys, uint32_t virt, uint32_t phys, uint32_t flags)
{
    uint32_t *dir = (uint32_t *)dir_phys;
    uint32_t pd_index = virt >> 22;
    uint32_t pt_index = (virt >> 12) & 0x3FF;

    /* Get or create page table.
     * PT frames are allocated from ZONE_LOW so they are within the identity-mapped
     * region and directly accessible by physical address. */
    if (!(dir[pd_index] & PTE_PRESENT)) {
        uint32_t pt_frame = pmm_alloc_frame_zone(PMM_ZONE_LOW);
        DBGK("vmm_map_page_in: new PT pdi=0x%lx pt=0x%lx virt=0x%lx dir=0x%lx\n",
             pd_index, pt_frame, virt, dir_phys);
        memset((void *)pt_frame, 0, PAGE_SIZE);
        dir[pd_index] = pt_frame | PTE_PRESENT | PTE_WRITE | (flags & PTE_USER);
    } else if (dir_phys != (uint32_t)page_directory &&
               (dir[pd_index] & 0xFFFFF000) == (page_directory[pd_index] & 0xFFFFF000)) {
        /* This task's PDE points to a shared kernel page table. Writing a
         * user mapping into it would corrupt all other tasks sharing that
         * table. Allocate a private copy for this task instead. */
        uint32_t kern_pt = dir[pd_index] & 0xFFFFF000;
        uint32_t pt_frame = pmm_alloc_frame_zone(PMM_ZONE_LOW);
        DBGK("vmm_map_page_in: private PT copy pdi=0x%lx kern_pt=0x%lx new_pt=0x%lx virt=0x%lx dir=0x%lx\n",
             pd_index, kern_pt, pt_frame, virt, dir_phys);
        memcpy((void *)pt_frame, (void *)kern_pt, PAGE_SIZE);
        dir[pd_index] = pt_frame | PTE_PRESENT | PTE_WRITE | (flags & PTE_USER);
    } else if ((flags & PTE_USER) && !(dir[pd_index] & PTE_USER)) {
        /* PDE exists (already private) but lacks user access flag on the PDE. */
        dir[pd_index] |= PTE_USER;
    }

    uint32_t *page_table = (uint32_t *)(dir[pd_index] & 0xFFFFF000);
    page_table[pt_index] = (phys & 0xFFFFF000) | (flags & 0xFFF);
}

void vmm_unmap_page_in(uint32_t dir_phys, uint32_t virt)
{
    uint32_t *dir = (uint32_t *)dir_phys;
    uint32_t pd_index = virt >> 22;
    uint32_t pt_index = (virt >> 12) & 0x3FF;

    if (virt >= 0xc0a10000 && virt < 0xc0a15000)
        DBGK("vmm_unmap_page_in: virt=0x%lx dir=0x%lx\n", virt, dir_phys);

    if (!(dir[pd_index] & PTE_PRESENT))
        return;

    uint32_t *page_table = (uint32_t *)(dir[pd_index] & 0xFFFFF000);
    page_table[pt_index] = 0;

    /* Flush TLB if this is the currently loaded page directory */
    uint32_t cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    if (cr3 == dir_phys)
        asm volatile("invlpg (%0)" : : "r"(virt) : "memory");
}

void vmm_map_page(uint32_t virt, uint32_t phys, uint32_t flags)
{
    vmm_map_page_in((uint32_t)page_directory, virt, phys, flags);
}

void vmm_unmap_page(uint32_t virt)
{
    vmm_unmap_page_in((uint32_t)page_directory, virt);
}

uint32_t vmm_get_phys(uint32_t virt)
{
    uint32_t pd_index = virt >> 22;
    uint32_t pt_index = (virt >> 12) & 0x3FF;

    if (!(page_directory[pd_index] & PTE_PRESENT))
        return 0;

    uint32_t *page_table = (uint32_t *)(page_directory[pd_index] & 0xFFFFF000);
    if (!(page_table[pt_index] & PTE_PRESENT))
        return 0;

    return (page_table[pt_index] & 0xFFFFF000) | (virt & 0xFFF);
}

uint32_t vmm_get_kernel_page_dir(void)
{
    return (uint32_t)page_directory;
}

/* Kernel virtual allocator: maps physical frames to virtual addresses
 * in the range 0xC0000000+.  Uses a free-list so vmm_free_pages()
 * reclaims virtual address space for reuse.
 *
 * Free-list entries are a static array of (base, npages) pairs.
 * On alloc: check free-list for an exact or larger range first,
 *           then fall back to bumping kvirt_next.
 * On free:  coalesce adjacent entries before inserting, reducing
 *           fragmentation and preventing list exhaustion.
 */
#define KVIRT_BASE      0xC0000000u
#define KVIRT_FL_MAX    64          /* max free-list entries */

static uint32_t kvirt_next = KVIRT_BASE;

struct virt_range {
    uint32_t base;
    uint32_t npages;
};

static struct virt_range kvirt_free[KVIRT_FL_MAX];
static uint32_t          kvirt_free_count = 0;

void *vmm_alloc_pages(uint32_t n)
{
    if (n == 0) return NULL;

    /* Check free-list for a range with exactly n pages (best fit) or
     * the smallest range >= n. */
    uint32_t best = KVIRT_FL_MAX;
    for (uint32_t i = 0; i < kvirt_free_count; i++) {
        if (kvirt_free[i].npages >= n) {
            if (best == KVIRT_FL_MAX ||
                kvirt_free[i].npages < kvirt_free[best].npages)
                best = i;
        }
    }

    uint32_t virt_base;
    if (best < KVIRT_FL_MAX) {
        virt_base = kvirt_free[best].base;
        uint32_t entry_end = kvirt_free[best].base + kvirt_free[best].npages * PAGE_SIZE;
        if (kvirt_free[best].npages == n) {
            /* Exact fit: remove entry */
            kvirt_free[best] = kvirt_free[--kvirt_free_count];
        } else {
            /* Larger: trim the front */
            kvirt_free[best].base   += n * PAGE_SIZE;
            kvirt_free[best].npages -= n;
        }
        /* Keep kvirt_next above the end of this recycled range so the bump
         * allocator never hands out an address that is already in use. */
        if (entry_end > kvirt_next)
            kvirt_next = entry_end;
    } else {
        /* Bump allocator */
        virt_base = kvirt_next;
        kvirt_next += n * PAGE_SIZE;
    }

    /* Map physical frames into the chosen virtual range */
    for (uint32_t i = 0; i < n; i++) {
        uint32_t frame = pmm_alloc_frame();
        if (!frame) return NULL;
        vmm_map_page(virt_base + i * PAGE_SIZE, frame, PTE_PRESENT | PTE_WRITE);
    }
    DBGK("alloc_pages %ld pages -> 0x%lx-0x%lx (hwm 0x%lx)\n",
         n, virt_base, virt_base + n * PAGE_SIZE, kvirt_next);
    return (void *)virt_base;
}

void vmm_free_pages(void *virt_base, uint32_t n)
{
    uint32_t v = (uint32_t)virt_base;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t phys = vmm_get_phys(v + i * PAGE_SIZE);
        if (phys) pmm_free_frame(phys);
        vmm_unmap_page(v + i * PAGE_SIZE);
    }

    /* Coalesce with any adjacent free-list entries, then insert.
     * Two ranges are adjacent if one ends exactly where the other begins.
     * Loop restarts after each merge because the swap-in of the last entry
     * may itself be adjacent to another entry. */
    uint32_t new_base   = v;
    uint32_t new_npages = n;

restart:
    for (uint32_t i = 0; i < kvirt_free_count; i++) {
        uint32_t e_end  = kvirt_free[i].base + kvirt_free[i].npages * PAGE_SIZE;
        uint32_t nw_end = new_base + new_npages * PAGE_SIZE;

        if (e_end == new_base) {
            /* existing entry is immediately before the new range */
            new_base   = kvirt_free[i].base;
            new_npages = kvirt_free[i].npages + new_npages;
            kvirt_free[i] = kvirt_free[--kvirt_free_count];
            goto restart;
        }
        if (nw_end == kvirt_free[i].base) {
            /* new range is immediately before the existing entry */
            new_npages += kvirt_free[i].npages;
            kvirt_free[i] = kvirt_free[--kvirt_free_count];
            goto restart;
        }
    }

    if (kvirt_free_count < KVIRT_FL_MAX) {
        kvirt_free[kvirt_free_count].base   = new_base;
        kvirt_free[kvirt_free_count].npages = new_npages;
        kvirt_free_count++;
    } else {
        printk("vmm: free-list full, losing virtual range 0x%lx (%ld pages)\n",
               new_base, new_npages);
    }
    DBGK("free_pages %ld pages @ 0x%lx (coalesced to 0x%lx+%ld, hwm 0x%lx)\n",
         n, v, new_base, new_npages, kvirt_next);
}

/* Temporary kmap pool: a fixed window of virtual pages at the top of the
 * kernel virtual range, separate from the vmm_alloc_pages pool so that
 * transient kmap slots can never collide with persistent allocations.
 * KMAP_SLOTS must be a power of two; 16 slots is plenty since vmm_clone_page_dir
 * only holds two kmaps at a time. */
#define KMAP_BASE  0xCFF00000u   /* top of the 256MB kernel virtual range */
#define KMAP_SLOTS 16

static uint16_t kmap_used = 0;  /* bitmask of in-use slots */

void *vmm_kmap(uint32_t phys)
{
    /* Find a free slot */
    for (int i = 0; i < KMAP_SLOTS; i++) {
        if (!(kmap_used & (1u << i))) {
            kmap_used |= (1u << i);
            uint32_t virt = KMAP_BASE + (uint32_t)i * PAGE_SIZE;
            /* Map into whichever page directory is currently loaded (CR3),
             * not the static kernel page_directory which may not be active. */
            uint32_t cr3;
            asm volatile("mov %%cr3, %0" : "=r"(cr3));
            vmm_map_page_in(cr3, virt, phys, PTE_PRESENT | PTE_WRITE);
            asm volatile("invlpg (%0)" : : "r"(virt) : "memory");
            return (void *)virt;
        }
    }
    panic("vmm_kmap: all slots in use");
    return NULL;
}

void vmm_kunmap(void *virt)
{
    uint32_t v = (uint32_t)virt;
    int slot = (int)((v - KMAP_BASE) / PAGE_SIZE);
    if (slot < 0 || slot >= KMAP_SLOTS)
        panic("vmm_kunmap: address not in kmap range");
    uint32_t cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    vmm_unmap_page_in(cr3, v);
    kmap_used &= ~(1u << slot);
}

/* Describe a kernel virtual address in terms of known regions.
 * Useful in fault handlers to give context without reading a map file. */
void vmm_describe_addr(uint32_t addr, char *buf, uint32_t buflen)
{
    (void)buflen;
    if (addr < KVIRT_BASE) {
        sprintf(buf, "user/physical (0x%lx)", addr);
        return;
    }
    if (addr >= KMAP_BASE && addr < KMAP_BASE + KMAP_SLOTS * PAGE_SIZE) {
        uint32_t slot = (addr - KMAP_BASE) / PAGE_SIZE;
        sprintf(buf, "kmap slot %ld (transient, 0x%lx)", slot, addr);
        return;
    }
    for (uint32_t i = 0; i < kvirt_free_count; i++) {
        uint32_t base = kvirt_free[i].base;
        uint32_t end  = base + kvirt_free[i].npages * PAGE_SIZE;
        if (addr >= base && addr < end) {
            sprintf(buf, "kvirt free [0x%lx-0x%lx) (0x%lx)", base, end, addr);
            return;
        }
    }
    if (addr < kvirt_next) {
        sprintf(buf, "kvirt alloc (0x%lx, below hwm 0x%lx)", addr, kvirt_next);
        return;
    }
    sprintf(buf, "kvirt unalloc (0x%lx, above hwm 0x%lx)", addr, kvirt_next);
}

uint32_t vmm_create_page_dir(void)
{
    /* Allocate from ZONE_LOW so the directory frame is within the
     * identity-mapped region and directly accessible by physical address. */
    uint32_t dir_frame = pmm_alloc_frame_zone(PMM_ZONE_LOW);
    DBGK("vmm_create_page_dir: dir_frame=0x%lx\n", dir_frame);
    uint32_t *dir = (uint32_t *)dir_frame;
    memcpy(dir, page_directory, PAGE_SIZE);
    return dir_frame;
}

uint32_t vmm_clone_page_dir(uint32_t src_dir_phys)
{
    /* Start with a fresh directory that inherits all kernel PDEs */
    uint32_t new_dir_phys = vmm_create_page_dir();
    uint32_t *src_dir = (uint32_t *)src_dir_phys;
    uint32_t *new_dir = (uint32_t *)new_dir_phys;

    /* Walk only the user-space PDE range */
    uint32_t user_pde_start = USER_SPACE_START >> 22;
    uint32_t user_pde_end   = USER_SPACE_END   >> 22;

    for (uint32_t pdi = user_pde_start; pdi < user_pde_end; pdi++) {
        if (!(src_dir[pdi] & PTE_PRESENT))
            continue;
        if (!(src_dir[pdi] & PTE_USER))
            continue;

        uint32_t src_pt_phys = src_dir[pdi] & 0xFFFFF000;
        uint32_t *src_pt = (uint32_t *)src_pt_phys;

        /* Allocate a private page table for the child (ZONE_LOW, directly accessible) */
        uint32_t new_pt_phys = pmm_alloc_frame_zone(PMM_ZONE_LOW);
        DBGK("vmm_clone_page_dir: new user PT pdi=0x%lx new_pt=0x%lx\n", pdi, new_pt_phys);
        memset((void *)new_pt_phys, 0, PAGE_SIZE);
        uint32_t *new_pt = (uint32_t *)new_pt_phys;

        for (uint32_t pti = 0; pti < 1024; pti++) {
            if (!(src_pt[pti] & PTE_PRESENT))
                continue;

            uint32_t frame = src_pt[pti] & 0xFFFFF000;
            uint32_t flags = src_pt[pti] & 0xFFF;

            if (flags & PTE_WRITE) {
                /* Writable page: mark both parent and child CoW, clear write bit.
                 * Increment refcount to track the shared frame. */
                flags = (flags & ~PTE_WRITE) | PTE_COW;
                src_pt[pti] = frame | flags;
                /* Flush parent's TLB entry so the write-protect takes effect */
                uint32_t virt = (pdi << 22) | (pti << 12);
                tlb_flush(virt);
            }
            /* Read-only pages (e.g. code) are shared as-is — no copy needed ever */

            pmm_refcount_inc(frame);
            new_pt[pti] = frame | flags;
        }

        /* Install the new page table in the child directory */
        new_dir[pdi] = new_pt_phys | (src_dir[pdi] & 0xFFF);
    }

    return new_dir_phys;
}

int vmm_cow_fault(uint32_t dir_phys, uint32_t fault_addr)
{
    uint32_t pdi = fault_addr >> 22;
    uint32_t pti = (fault_addr >> 12) & 0x3FF;

    uint32_t *dir = (uint32_t *)dir_phys;
    if (!(dir[pdi] & PTE_PRESENT))
        return 0;

    uint32_t *pt = (uint32_t *)(dir[pdi] & 0xFFFFF000);
    if (!(pt[pti] & PTE_PRESENT))
        return 0;
    if (!(pt[pti] & PTE_COW))
        return 0;

    uint32_t old_frame = pt[pti] & 0xFFFFF000;
    uint32_t flags     = pt[pti] & 0xFFF;

    DBGK("cow_fault: virt=0x%lx old_frame=0x%lx refcount=%lu dir_phys=0x%lx\n",
         fault_addr, old_frame, pmm_refcount_get(old_frame), dir_phys);

    if (pmm_refcount_get(old_frame) == 1) {
        /* Sole owner: just make it writable, no copy needed */
        pt[pti] = old_frame | ((flags & ~PTE_COW) | PTE_WRITE);
    } else {
        /* Shared (refcount > 1): allocate new frame, copy content, remap.
         * Release our reference to the old frame. */
        uint32_t new_frame = pmm_alloc_frame();
        DBGK("cow_fault: new_frame=0x%lx\n", new_frame);
        void *old_kp = vmm_kmap(old_frame);
        void *new_kp = vmm_kmap(new_frame);
        memcpy(new_kp, old_kp, PAGE_SIZE);
        vmm_kunmap(new_kp);
        vmm_kunmap(old_kp);

        pmm_refcount_dec(old_frame);
        pt[pti] = new_frame | ((flags & ~PTE_COW) | PTE_WRITE);
    }

    tlb_flush(fault_addr & 0xFFFFF000);
    return 1;
}

void vmm_destroy_page_dir(uint32_t dir_phys)
{
    if (dir_phys == (uint32_t)page_directory)
        panic("vmm_destroy_page_dir: cannot destroy kernel page directory");

    /* dir_phys is from ZONE_LOW — directly accessible. Page table frames are
     * also from ZONE_LOW. User data frames pointed to by PTEs are from ZONE_ANY
     * and don't need to be accessed here, only freed. */
    uint32_t *dir = (uint32_t *)dir_phys;

    for (int i = 0; i < 1024; i++) {
        if (!(dir[i] & PTE_PRESENT))
            continue;

        /* Skip entries that exactly match the kernel template */
        if (dir[i] == page_directory[i])
            continue;

        uint32_t dir_frame  = dir[i] & 0xFFFFF000;
        uint32_t kern_frame = page_directory[i] & 0xFFFFF000;
        uint32_t *pt = (uint32_t *)dir_frame;

        if ((page_directory[i] & PTE_PRESENT) && dir_frame == kern_frame) {
            /* Shared kernel page table — only release user-mapped PTEs */
            for (int j = 0; j < 1024; j++) {
                if ((pt[j] & PTE_PRESENT) && (pt[j] & PTE_USER)) {
                    uint32_t frame = pt[j] & 0xFFFFF000;
                    if (pmm_refcount_dec(frame) == 0)
                        pmm_free_frame(frame);
                }
            }
        } else {
            /* Task-private page table (from ZONE_LOW, directly accessible) —
             * release user-mapped frames only; kernel frames are shared and
             * must not be freed here. Then free the table itself. */
            for (int j = 0; j < 1024; j++) {
                if ((pt[j] & PTE_PRESENT) && (pt[j] & PTE_USER)) {
                    uint32_t frame = pt[j] & 0xFFFFF000;
                    if (pmm_refcount_dec(frame) == 0)
                        pmm_free_frame(frame);
                }
            }
            pmm_free_frame(dir_frame);
        }
    }

    pmm_free_frame(dir_phys);
}

void vmm_switch_page_dir(uint32_t dir_phys)
{
    asm volatile("mov %0, %%cr3" : : "r"(dir_phys) : "memory");
}

/* Identity-map a range of physical addresses (page-aligned) */
static void identity_map_range(uint32_t start, uint32_t end, uint32_t flags)
{
    /* Align start down, end up to page boundaries */
    start &= 0xFFFFF000;
    end = (end + PAGE_SIZE - 1) & 0xFFFFF000;

    for (uint32_t addr = start; addr < end; addr += PAGE_SIZE)
        vmm_map_page(addr, addr, flags);
}

void init_vmm(void)
{
    /* Zero the page directory */
    memset(page_directory, 0, sizeof(page_directory));

    /* Identity-map 0-16MB: covers kernel, stack, heap, VGA, and all of
     * ZONE_LOW (0x100000-0x1000000). All structural frames (page dirs,
     * page tables) are allocated from ZONE_LOW and must be directly
     * accessible by physical address. */
    identity_map_range(0x00000000, 0x01000000, PTE_PRESENT | PTE_WRITE);

    /* Identity-map the VBE framebuffer region if available.
     * Typically at 0xFD000000, roughly 3-4MB in size.
     */
    if (mb_info->vbe_mode_info) {
        struct vbe_mode_info_struct *vbe = mb_info->vbe_mode_info;
        if (vbe->framebuffer) {
            uint32_t fb_start = vbe->framebuffer;
            /* pitch * height gives the actual framebuffer size */
            uint32_t fb_size = (uint32_t)vbe->pitch * vbe->height;
            if (fb_size == 0)
                fb_size = 4 * 1024 * 1024; /* fallback: 4MB */
            printk(MODULE "mapping VBE framebuffer 0x%lx - 0x%lx (%ld KB)\n",
                   fb_start, fb_start + fb_size, fb_size / 1024);
            identity_map_range(fb_start, fb_start + fb_size,
                               PTE_PRESENT | PTE_WRITE);
        }
    }

    /* Pre-allocate page tables for all managed physical memory, plus the
     * kernel virtual allocator region at 0xC0000000-0xCFFFFFFF (64 PDEs =
     * 256MB of kernel virtual space).
     *
     * Must be done before paging is enabled: PT frames are accessed at their
     * physical address. PT frames come from ZONE_LOW which is within the
     * identity-mapped range we just established above. */
    auto void prealloc_pt(uint32_t pd_index);
    auto void prealloc_pt(uint32_t pd_index) {
        if (!(page_directory[pd_index] & PTE_PRESENT)) {
            uint32_t pt_frame = pmm_alloc_frame_zone(PMM_ZONE_LOW);
            memset((void *)pt_frame, 0, PAGE_SIZE);
            page_directory[pd_index] = pt_frame | PTE_PRESENT | PTE_WRITE;
            vmm_map_page(pt_frame, pt_frame, PTE_PRESENT | PTE_WRITE);
        }
    }

    /* Physical memory range: one PDE per 4MB, driven by pmm_total_frames() */
    uint32_t phys_top = pmm_total_frames() * PAGE_SIZE;
    for (uint32_t addr = 0; addr < phys_top; addr += 4 * 1024 * 1024)
        prealloc_pt(addr >> 22);

    /* Kernel virtual allocator range: 0xC0000000-0xCFFFFFFF (64 PDEs, 256MB) */
    for (uint32_t pdi = 0xC0000000 >> 22; pdi < (0xC0000000 >> 22) + 64; pdi++)
        prealloc_pt(pdi);

    printk(MODULE "page directory at 0x%lx\n", (uint32_t)page_directory);

    /* Load page directory into CR3 */
    asm volatile("mov %0, %%cr3" : : "r"(page_directory));

    /* Enable paging (set CR0.PG, bit 31) */
    uint32_t cr0;
    asm volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= 0x80000000;
    asm volatile("mov %0, %%cr0" : : "r"(cr0));

}
