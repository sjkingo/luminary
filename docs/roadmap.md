# Luminary OS — Roadmap & Known Issues

## What Luminary Has (vs Ulysses)

- VBE framebuffer graphics with bitmap font rendering
- PCI bus enumeration and driver registry
- RTL8139 NIC initialisation
- Priority-based scheduling with dynamic aging (vs round-robin)
- Modular Makefile build (vs SCons)
- Python codegen for interrupt vectors
- GRUB2 support (vs GRUB legacy)
- User mode (ring 3) with TSS, per-task page directories
- ELF32 loader
- CPIO initrd, VFS layer, path-based file access
- fork/exec/waitpid process model
- Syscall interface (`int 0x80`, 31 syscalls)
- PS/2 keyboard driver with ring buffer
- PS/2 mouse driver (IRQ12, absolute position tracking)
- GUI compositor: three-buffer rendering, window management, drag, resize, close, statusbar/taskbar, focus-follows-mouse, resize cursor sprites, console window
- Interactive shell (`sh`) with VFS commands and path-based exec
- `vmm_alloc_pages`: maps non-contiguous physical frames to contiguous kernel virtual range at `0xC0000000+`, with free-list reclaim
- PMM zones: ZONE_LOW for contiguous/DMA allocations, ZONE_ANY for general use
- `pmm_alloc_contiguous(n)`: allocates n physically contiguous frames from ZONE_LOW
- PMM initialised from Multiboot memory map, supports up to 4GB (was 128MB)
- Slab allocator (`kmalloc`/`kfree`): 8 size classes (32–4096 bytes), PMM-backed, with overflow path for large allocations via `vmm_alloc_pages`
- Kernel symbol table (two-pass build): stack traces resolve addresses to `function (file:line)` for CPU exceptions
- `fork()`/`exec()` with full address space copy (`vmm_clone_page_dir`)

## What Luminary Needs

1. **Network stack** — build on RTL8139 driver (has init but no packet I/O or IRQ handler yet)
2. **i3-style keybinding system** — planned, not yet started
3. **Writable filesystem** — initrd is read-only; no mechanism to create/write files

## Known Bugs

1. **VGA scroll underflow**: `scroll_screen()` accesses `vid.buffer[i-VGA_WIDTH]` when `i < VGA_WIDTH` (first row), causing buffer underflow. Fix when working in `vga.c`.

2. **fbdev.h type mismatch**: Header declares `depth` and `pitch` as `uint32_t`, but implementation uses `uint8_t` and `uint16_t`. Fix when working in `fbdev.c`/`fbdev.h`.

3. **GUI back buffer frame leak**: PMM frames for the full-screen `back` buffer are allocated once in `init_gui()` and never freed (compositor exit does not free them — intentional for performance). Acceptable for the lifetime of the kernel.

4. **Back buffer allocation failure**: `pmm_alloc_contiguous(n)` allocates the back buffer from ZONE_LOW. If ZONE_LOW is exhausted (unlikely at init time), the GUI falls back to drawing directly to `fb_hw` with degraded performance (no partial updates, cursor drawn into scene).

5. **`struct task` offset constants**: `TASK_ESP_OFFSET`, `TASK_PAGE_DIR_OFFSET`, `TASK_STACK_BASE_OFFSET` in `task.h` must match the actual byte layout of `struct task`. Verified with `_Static_assert(offsetof(...))`. If `struct task` is modified, update both the constants and the corresponding defines in `cpu/traps.S`. A `cpu/traps.o: kernel/task.h` dependency in the Makefile ensures `traps.S` is rebuilt when `task.h` changes.
