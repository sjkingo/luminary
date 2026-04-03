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
- `vmm_alloc_pages`: maps non-contiguous physical frames to contiguous kernel virtual range at `0xC0000000+`

## What Luminary Needs

1. **Better heap** — `heap.c` is a fixed-block 1MB allocator with poor large-allocation support and fragmentation. Should be replaced with a size-class or buddy allocator. GUI backbuffers have been moved to PMM/vmm_alloc_pages to avoid heap pressure, but other large kernel allocations remain constrained.
2. **Network stack** — build on RTL8139 driver (has init but no packet I/O or IRQ handler yet)
3. **i3-style keybinding system** — planned, not yet started
4. **Writable filesystem** — initrd is read-only; no mechanism to create/write files

## Known Bugs

1. **VGA scroll underflow**: `scroll_screen()` accesses `vid.buffer[i-VGA_WIDTH]` when `i < VGA_WIDTH` (first row), causing buffer underflow. Fix when working in `vga.c`.

2. **fbdev.h type mismatch**: Header declares `depth` and `pitch` as `uint32_t`, but implementation uses `uint8_t` and `uint16_t`. Fix when working in `fbdev.c`/`fbdev.h`.

3. **GUI back buffer frame leak**: PMM frames for the full-screen `back` buffer are allocated once in `init_gui()` and never freed (compositor exit does not free them — intentional for performance, but leaks ~3MB on each full kernel lifetime). Acceptable for now; fix if multiple compositor lifecycles become an issue.

4. **bb_alloc contiguous requirement**: Per-window backbuffers currently use `vmm_alloc_pages` (non-contiguous physical frames, contiguous virtual). If PMM is heavily fragmented, physical frames may be scattered, which is handled correctly. However, the full-scene back buffer still requires contiguous physical frames (allocated early in `init_gui` before fragmentation). If this allocation fails, the compositor falls back to drawing directly to `fb_hw` with degraded performance.

5. **`struct task` offset constants**: `TASK_ESP_OFFSET`, `TASK_PAGE_DIR_OFFSET`, `TASK_STACK_BASE_OFFSET` in `task.h` must match the actual byte layout of `struct task`. Verified with `_Static_assert(offsetof(...))`. If `struct task` is modified, update both the constants and the corresponding defines in `cpu/traps.S`. A `cpu/traps.o: kernel/task.h` dependency in the Makefile ensures `traps.S` is rebuilt when `task.h` changes.
