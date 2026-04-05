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
- Syscall interface (`int 0x80`, 27 syscalls including `mkdir`/`unlink`, `getppid`, `waitpid` with WNOHANG and exit-status propagation; device-specific operations via `SYS_IOCTL` on `/dev/x` and `/dev/sys`)
- `SYS_FCNTL` (44): `F_GETFL`/`F_SETFL` with `O_NONBLOCK` per-fd flag; `SYS_READ` respects the fd flag without needing `SYS_READ_NB`
- `SYS_MOUNT` (46) / `SYS_UMOUNT` (47): mount/unmount registered filesystem drivers at arbitrary VFS paths; `umount` refuses if nested mounts exist under the target path
- VFS read-only enforcement: initrd root marked readonly after mount; `vfs_creat`/`vfs_mkdir`/`vfs_unlink`/`vfs_write` return -1 for readonly directories
- VFS filesystem driver framework: `struct fs_ops` vtable (`mount`/`umount`), `vfs_fs_register()`, `vfs_do_mount()`/`vfs_do_umount()`; `vfs_lookup` transparently follows `mounted_root` pointers at each traversal step
- tmpfs (`src/kernel/tmpfs.c`): registered filesystem driver; `mount` allocates a fresh writable VFS subtree, `umount` recursively frees it; mounted at `/tmp` on boot via `vfs_do_mount`
- PS/2 keyboard driver with ring buffer
- PS/2 mouse driver (IRQ12, absolute position tracking)
- GUI compositor: three-buffer rendering, window management, drag, resize, close, statusbar/taskbar, focus-follows-mouse, resize cursor sprites, console window
- Interactive shell (`sh`) with VFS commands, path-based exec, pipelines, I/O redirection, background jobs (`&`), `fg`/`jobs` builtins, and Ctrl+C handling that only kills foreground children
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
- Per-task CPU accounting: `ticks` and `ticks_window` fields in `struct task`; scheduler increments them on every timer tick for the running task (only when `blocking=false`); `total_ticks` global tracks all ticks since boot; 1-second rolling window (`ticks_window` reset every 1000 ticks) gives `cpu_pct` per task
- `ps`: tree-view process listing with PID, PPID, PRI, TIME (elapsed since task creation), CPU% (1s rolling window), state (`I`dle/`R`un/`B`lock/`W`ait/`S`usp/`D`ispatchable), and CMD; summary line shows total CPU%, idle%, and uptime in `h:mm:ss` format
- `uptime`: prints system uptime in `h:mm:ss` format

## What Luminary Needs

1. **Network stack** — build on RTL8139 driver (has init but no packet I/O or IRQ handler yet)
2. **i3-style keybinding system** — planned, not yet started
3. **Persistent filesystem** — `/tmp` is writable but non-persistent. Implementing ext2 or FAT as a `struct fs_ops` driver would give persistence; the mount framework is ready to host it.
4. **Block device abstraction** — prerequisite for persistent fs; need a `struct blkdev` with `read_blocks`/`write_blocks` ops, registered per device (IDE/ATA or virtio-blk)

## Known Bugs

1. **GUI back buffer frame leak**: PMM frames for the full-screen `back` buffer are allocated once in `init_gui()` and never freed (compositor exit does not free them — intentional for performance). Acceptable for the lifetime of the kernel.

2. **Back buffer allocation failure**: `pmm_alloc_contiguous(n)` allocates the back buffer from ZONE_LOW. If ZONE_LOW is exhausted (unlikely at init time), the GUI falls back to drawing directly to `fb_hw` with degraded performance (no partial updates, cursor drawn into scene).

3. **RTL8139 no packet I/O**: NIC interrupts are masked (IMR=0x0000) until a real IRQ handler is registered. The chip initialises but cannot send or receive packets.

4. **`struct task` offset constants**: `TASK_ESP_OFFSET`, `TASK_PAGE_DIR_OFFSET`, `TASK_STACK_BASE_OFFSET` in `task.h` must match the actual byte layout of `struct task`. Verified with `_Static_assert(offsetof(...))`. If `struct task` is modified, update both the constants and the corresponding defines in `cpu/traps.S`. A `cpu/traps.o: kernel/task.h` dependency in the Makefile ensures `traps.S` is rebuilt when `task.h` changes.

5. **`kill 1` GPF on respawned init's first fork**: Running `kill 1` kills init and all its children, then respawns init. When the new init forks its first child (sh), the child GPFs at `iret` with DS=0x0000 and zeroed registers. Debug instrumentation confirmed the child's kernel stack trap frame (virt `0xc0a14fbc`, phys `0x1a26000`) is valid immediately after `task_fork` returns, but is fully zeroed by the time the scheduler switches to it (the PTE's dirty bit is set, confirming a write occurred). The PTE mapping itself remains intact. Suspected cause: `vmm_destroy_page_dir` called during the kill cascade decrements CoW refcounts; if a refcount incorrectly hits zero, `pmm_free_frame` is called on a frame still mapped as the child's kstack page, allowing the PMM to hand it out again and zero it (e.g. as a new page table). Investigate `vmm_destroy_page_dir` CoW refcount handling and the interaction with `kstack_alloc` frames.
