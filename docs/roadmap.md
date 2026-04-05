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
- Syscall interface (`int 0x80`, 25 syscalls including `mkdir`/`unlink`, `getppid`, `waitpid` with WNOHANG and exit-status propagation; device-specific operations via `SYS_IOCTL` on `/dev/x` and `/dev/sys`)
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
- `fork()`/`exec()` with copy-on-write address space cloning (`vmm_clone_page_dir`): writable pages marked CoW, read-only pages shared; refcounts track sharing
- Character device abstraction (`VFS_CHARDEV`): `/dev/stdin`, `/dev/stdout`, `/dev/stderr` as VFS nodes; `read(fd,buf,len)`/`write(fd,buf,len)` dispatch through fd table; fds 0/1/2 pre-opened on every task
- Anonymous pipes (`pipe()`/`dup2()`): 4KB ring buffer, up to 16 concurrent pipes, blocking read/write with EOF and broken-pipe semantics; enables shell I/O redirection; `write_refs`/`read_refs` refcounts allow correct sharing across `fork()` and `dup2()`
- GUI terminal emulator (`/bin/term`): userland process that creates a window, forks `/bin/sh`, connects it via pipes, and renders output; multiple term instances can run simultaneously
- Ctrl+C signal interrupts: keyboard driver emits `\x03`; shell uses interruptible wait (`task_done()` + `read_nb()` + `yield()`) to detect and kill children
- `task_death_hook`: registered callback fired by `task_kill()` for per-task resource cleanup; GUI uses it to destroy orphaned windows
- Kernel stack guard pages: each task's 16KB kernel stack has an unmapped guard page immediately below it; overflow triggers a page fault rather than silent corruption

## What Luminary Needs

1. **Network stack** — build on RTL8139 driver (has init but no packet I/O or IRQ handler yet)
2. **i3-style keybinding system** — planned, not yet started
3. **Writable filesystem** — initrd is read-only; no mechanism to create/write files
4. **SYS_SPAWN** — there is no syscall to create a new independent task from user space; the GUI currently spawns `/bin/sh` etc. directly via kernel calls. A `spawn(path)` syscall (returns new pid, no parent relationship) would enable userland process launchers and the taskbar to start programs without fork+exec.
5. **VFS tmpfs** — `/tmp` is currently backed by initrd nodes. A proper tmpfs (in-memory, pre-mounted at `/tmp`, supports `mkdir`/`unlink`/`creat` on a fresh tree) would give programs a clean scratch space that doesn't consume initrd node pool slots.
6. **O_NONBLOCK / SYS_FCNTL** — there is no per-fd non-blocking flag. Non-blocking reads are done via `SYS_READ_NB` (a separate syscall). POSIX programs expect `fcntl(fd, F_SETFL, O_NONBLOCK)` to set the flag on the fd itself, and `read(fd, …)` to then return EAGAIN rather than blocking. Implementing `SYS_FCNTL` with `F_GETFL`/`F_SETFL` and adding an `O_NONBLOCK` bit to `struct vfs_fd` would make porting POSIX code easier.

## Known Bugs

1. **VGA scroll underflow**: `scroll_screen()` accesses `vid.buffer[i-VGA_WIDTH]` when `i < VGA_WIDTH` (first row), causing buffer underflow. Fix when working in `vga.c`.

2. **fbdev.h type mismatch**: Header declares `depth` and `pitch` as `uint32_t`, but implementation uses `uint8_t` and `uint16_t`. Fix when working in `fbdev.c`/`fbdev.h`.

3. **GUI back buffer frame leak**: PMM frames for the full-screen `back` buffer are allocated once in `init_gui()` and never freed (compositor exit does not free them — intentional for performance). Acceptable for the lifetime of the kernel.

4. **Back buffer allocation failure**: `pmm_alloc_contiguous(n)` allocates the back buffer from ZONE_LOW. If ZONE_LOW is exhausted (unlikely at init time), the GUI falls back to drawing directly to `fb_hw` with degraded performance (no partial updates, cursor drawn into scene).

5. **RTL8139 interrupts enabled with no handler**: `init_rtl8139()` enables NIC interrupts (IMR register) but the IRQ handler is a stub (`// TODO`). Network interrupts will fire and be silently dropped by the PIC spurious-IRQ path. No packet I/O is possible.

6. **`struct task` offset constants**: `TASK_ESP_OFFSET`, `TASK_PAGE_DIR_OFFSET`, `TASK_STACK_BASE_OFFSET` in `task.h` must match the actual byte layout of `struct task`. Verified with `_Static_assert(offsetof(...))`. If `struct task` is modified, update both the constants and the corresponding defines in `cpu/traps.S`. A `cpu/traps.o: kernel/task.h` dependency in the Makefile ensures `traps.S` is rebuilt when `task.h` changes.

7. **`kill 1` GPF on respawned init's first fork**: Running `kill 1` kills init and all its children, then respawns init. When the new init forks its first child (sh), the child GPFs at `iret` with DS=0x0000 and zeroed registers. Debug instrumentation confirmed the child's kernel stack trap frame (virt `0xc0a14fbc`, phys `0x1a26000`) is valid immediately after `task_fork` returns, but is fully zeroed by the time the scheduler switches to it (the PTE's dirty bit is set, confirming a write occurred). The PTE mapping itself remains intact. Suspected cause: `vmm_destroy_page_dir` called during the kill cascade decrements CoW refcounts; if a refcount incorrectly hits zero, `pmm_free_frame` is called on a frame still mapped as the child's kstack page, allowing the PMM to hand it out again and zero it (e.g. as a new page table). Investigate `vmm_destroy_page_dir` CoW refcount handling and the interaction with `kstack_alloc` frames.
