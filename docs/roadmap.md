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
- Syscall interface (`int 0x80`, 50 syscalls including `mkdir`/`unlink`, `getppid`, `waitpid` with WNOHANG and exit-status propagation; device-specific operations via `SYS_IOCTL` on `/dev/x`, `/dev/sys`, `/dev/fb0`, and block device nodes; `SYS_FSTAT` (48), `SYS_RENAME` (49), `SYS_BRK` (50))
- `SYS_FCNTL` (44): `F_GETFL`/`F_SETFL` with `O_NONBLOCK` per-fd flag; `SYS_READ` respects the fd flag without needing `SYS_READ_NB`
- `SYS_MOUNT` (46) / `SYS_UMOUNT` (47): mount/unmount registered filesystem drivers at arbitrary VFS paths; `umount` refuses if nested mounts exist under the target path
- VFS read-only enforcement: initrd root marked readonly after mount; `vfs_creat`/`vfs_mkdir`/`vfs_unlink`/`vfs_write` return -1 for readonly directories
- VFS filesystem driver framework: `struct fs_ops` vtable (`mount`/`umount`), `vfs_fs_register()`, `vfs_do_mount()`/`vfs_do_umount()`; `vfs_lookup` transparently follows `mounted_root` pointers at each traversal step
- tmpfs (`src/kernel/tmpfs.c`): registered filesystem driver; `mount` allocates a fresh writable VFS subtree, `umount` recursively frees it; mounted at `/tmp` on boot via `vfs_do_mount`
- initrd (`src/kernel/initrd.c`): registered filesystem driver (`"initrd"`); `init_initrd(data, size)` saves the cpio pointer and registers ops; `mount` re-parses the cpio newc archive zero-copy into a VFS subtree; `umount` frees node tree without touching the data buffer; boot uses `vfs_do_mount("/", "initrd")`; userland can `mount initrd /path` to expose the archive at any mountpoint; kernel panics if `root=` and an initrd module are both present
- Kernel cmdline parsing (`src/kernel/cmdline.c`): GRUB2 multiboot cmdline split into key=value tokens; `cmdline_get(key)` for lookups; `cmdline_raw()` for display; shown in startup banner
- MBR partition probing (`drivers/part.c`): reads sector 0, validates 0x55AA signature, registers up to 4 primary partitions as child blkdevs (`hda1`–`hda4`)
- Dual boot paths: initrd (no `root=`) or block device (`root=/dev/hdXN`); `make qemu` boots GRUB2 ISO with disk image attached; GRUB menu switches between the two
- PS/2 keyboard driver with ring buffer
- PS/2 mouse driver (IRQ12, absolute position tracking)
- GUI compositor: three-buffer rendering, window management, drag, resize, close, statusbar/taskbar, focus-follows-mouse, resize cursor sprites, console window
- Interactive shell (`sh`) with VFS commands, path-based exec, pipelines, I/O redirection, background jobs (`&`), `fg`/`jobs` builtins, Ctrl+C handling that only kills foreground children, glob expansion (`*` in any token, expanded before exec/pipe, no-match pass-through), and a readline line editor with history (100-entry ring, consecutive-duplicate suppression), insert mode, left/right/home/end/delete cursor movement, and up/down history navigation
- `vmm_alloc_pages`: maps non-contiguous physical frames to contiguous kernel virtual range at `0xC0000000+`, with free-list reclaim
- PMM zones: ZONE_LOW for contiguous/DMA allocations, ZONE_ANY for general use
- `pmm_alloc_contiguous(n)`: allocates n physically contiguous frames from ZONE_LOW
- PMM initialised from Multiboot memory map, supports up to 4GB (was 128MB)
- Slab allocator (`kmalloc`/`kfree`): 8 size classes (32–4096 bytes), PMM-backed, with overflow path for large allocations via `vmm_alloc_pages`
- Kernel symbol table (two-pass build): stack traces resolve addresses to `function (file:line)` for CPU exceptions
- `fork()`/`exec()` with copy-on-write address space cloning (`vmm_clone_page_dir`): writable pages marked CoW, read-only pages shared; refcounts track sharing
- Character device abstraction (`VFS_CHARDEV`): `/dev/stdin`, `/dev/stdout`, `/dev/stderr` as VFS nodes; `read(fd,buf,len)`/`write(fd,buf,len)` dispatch through fd table; fds 0/1/2 pre-opened on every task
- Anonymous pipes (`pipe()`/`dup2()`): 4KB ring buffer, up to 16 concurrent pipes, blocking read/write with EOF and broken-pipe semantics; enables shell I/O redirection; `write_refs`/`read_refs` refcounts allow correct sharing across `fork()` and `dup2()`; pipe refcount race condition fixed — `sys_fork` increments refcounts on the parent's fd table before `task_fork` (not after), so the child cannot race and close fds before the parent records the inheritance
- GUI terminal emulator (`/bin/term`): userland process that creates a window, forks `/bin/sh`, connects it via pipes, and renders output using the shared `termemu` library; multiple term instances can run simultaneously; arrow keys decoded to ANSI escape sequences by `term`
- Framebuffer console daemon (`/bin/fbcon`): userland console daemon that reads framebuffer geometry from `/dev/fb0`, forks `/bin/sh`, relays keyboard input, renders output via the shared `termemu` + `fbrender` libraries; init execs fbcon instead of sh directly; Page Up/Down handled locally for scrollback with burst coalescing (all pending scroll events drained before a single render pass); rendering suppressed while GUI compositor owns keyboard (`SYS_CTL_GUI_ACTIVE` on `/dev/sys`) to avoid stomping on GUI output; shadow buffer used — rendering targets RAM, then memcpy to MMIO framebuffer for each dirty row range
- `/dev/console`: kernel pipe whose write end is driven by `stdout_write_op`; fbcon reads the read end to display early-boot printk output alongside shell output; non-blocking writes (bytes dropped if pipe full) preserve kernel correctness
- `/dev/fb0`: ioctl chardev exposing VBE framebuffer geometry (`FBIOGET_INFO` returns width/height/pitch/depth/fb_addr); VBE framebuffer identity-mapped with `PTE_USER` so userland can write to it directly
- Shared `termemu` library (`userland/libc/termemu.c`): ring-buffer terminal emulator (scrollback, cursor tracking, dirty rows) shared by fbcon and term; `struct termemu` with `termemu_putchar`, `termemu_scroll_up/down`, `termemu_get_visible_row`, dirty row API
- Arrow key sentinel bytes (`KEY_UP`–`KEY_DEL`, 0x10–0x16) decoded in keyboard driver and translated to ANSI escape sequences by `term`; passed through raw to fbcon's shell pipe and consumed by `sh`'s readline line editor (`userland/libc/readline.c`); `stdin_read_op` passes all sentinels 0x10–0x16 through to userland, discarding only `KEY_ALT_F4` (0x17)
- Ctrl+C signal interrupts: keyboard driver emits `\x03`; shell uses interruptible wait (`task_done()` + `read_nb()` + `yield()`) to detect and kill children
- `task_death_hook`: registered callback fired by `task_kill()` for per-task resource cleanup; GUI uses it to destroy orphaned windows
- Kernel stack guard pages: each task's 16KB kernel stack has an unmapped guard page immediately below it; overflow triggers a page fault rather than silent corruption
- Per-task CPU accounting: `ticks` and `ticks_window` fields in `struct task`; scheduler increments them on every timer tick for the running task (only when `blocking=false`); `total_ticks` global tracks all ticks since boot; 1-second rolling window (`ticks_window` reset every 1000 ticks) gives `cpu_pct` per task
- `ps`: tree-view process listing with PID, PPID, PRI, TIME (elapsed since task creation), CPU% (1s rolling window), state (`I`dle/`R`un/`B`lock/`W`ait/`S`usp/`D`ispatchable), and CMD; summary line shows total CPU%, idle%, and uptime in `h:mm:ss` format
- `uptime`: prints system uptime in `h:mm:ss` format
- Block device abstraction (`drivers/blkdev.c`): registry of up to 8 devices; byte-offset VFS chardev ops with sector-granular LBA translation; per-slot function technique (same as pipes) for context-free VFS op dispatch; `BLKDEV_IOCTL_GETSIZE`/`BLKDEV_IOCTL_GETSECTSZ`
- ATA PIO driver (`drivers/ata.c`): probes primary and secondary IDE channels; IDENTIFY-based detection; LBA28 read/write; drive interrupts masked (nIEN); drives registered as `/dev/hda`–`/dev/hdd`
- VFS improvements: `vfs_alloc_node` falls back to `kmalloc` when the 512-node static pool is exhausted (heap-allocated nodes freed via `kfree`); inode numbers populated from cpio headers for initrd nodes and auto-assigned for runtime-created nodes; `struct vfs_stat` exposes inode; `vfs_rename` with Linux semantics; `mv` userland program
- `stat` shows inode number; `fstat(fd, &st)` via `SYS_FSTAT (48)`
- ext2 filesystem driver (`src/fs/ext2.c`): registered as `"ext2"`; `ext2_mount` reads the superblock and block group descriptors, eagerly walks the directory tree from inode 2, and populates the VFS tree; file data read/written on demand via per-slot op functions (same pattern as blkdev/pipe); write support for already-allocated blocks (no block allocation or bitmap updates); `ext2_umount` frees the VFS subtree and clears the slot table; boot with `root=/dev/hda1` in the GRUB entry or `mount ext2 /dev/hda1 /mnt` from userland

## What Luminary Needs

1. **Network stack** — build on RTL8139 driver (has init but no packet I/O or IRQ handler yet)
2. **i3-style keybinding system** — planned, not yet started
3. **ext2 block allocation** — write support currently only handles already-allocated blocks; growing files or creating new ones requires bitmap updates and block allocation in the ext2 driver
4. **ATA LBA48** — current ATA driver is LBA28 only (max 128GB). LBA48 support requires using commands `0x24`/`0x34` and writing the high sector count and LBA bytes via the HOB register sequence.
5. **ANSI colour output in fbcon/termemu** — `termemu` has no CSI parser; colour prompts and ls colour output are not rendered; CSI SGR sequences should set per-cell fg/bg colour attributes
6. **sh history persistence** — history ring is in-memory only; not persisted across shell sessions
7. **Early-boot printk lost if pipe full** — bytes written to `/dev/console` pipe before fbcon starts are dropped if the pipe's 4KB ring fills; serial output captures everything but framebuffer does not show early boot messages

## Known Bugs

1. **GUI back buffer frame leak**: PMM frames for the full-screen `back` buffer are allocated once in `init_gui()` and never freed (compositor exit does not free them — intentional for performance). Acceptable for the lifetime of the kernel.

2. **Back buffer allocation failure**: `pmm_alloc_contiguous(n)` allocates the back buffer from ZONE_LOW. If ZONE_LOW is exhausted (unlikely at init time), the GUI falls back to drawing directly to `fb_hw` with degraded performance (no partial updates, cursor drawn into scene).

3. **RTL8139 no packet I/O**: NIC interrupts are masked (IMR=0x0000) until a real IRQ handler is registered. The chip initialises but cannot send or receive packets.

4. **`struct task` offset constants**: `TASK_ESP_OFFSET`, `TASK_PAGE_DIR_OFFSET`, `TASK_STACK_BASE_OFFSET` in `task.h` must match the actual byte layout of `struct task`. Verified with `_Static_assert(offsetof(...))`. If `struct task` is modified, update both the constants and the corresponding defines in `cpu/traps.S`. A `cpu/traps.o: kernel/task.h` dependency in the Makefile ensures `traps.S` is rebuilt when `task.h` changes.

5. **`kill 1` GPF on respawned init's first fork**: Running `kill 1` kills init and all its children, then respawns init. When the new init forks its first child (sh), the child GPFs at `iret` with DS=0x0000 and zeroed registers. Debug instrumentation confirmed the child's kernel stack trap frame (virt `0xc0a14fbc`, phys `0x1a26000`) is valid immediately after `task_fork` returns, but is fully zeroed by the time the scheduler switches to it (the PTE's dirty bit is set, confirming a write occurred). The PTE mapping itself remains intact. Suspected cause: `vmm_destroy_page_dir` called during the kill cascade decrements CoW refcounts; if a refcount incorrectly hits zero, `pmm_free_frame` is called on a frame still mapped as the child's kstack page, allowing the PMM to hand it out again and zero it (e.g. as a new page table). Investigate `vmm_destroy_page_dir` CoW refcount handling and the interaction with `kstack_alloc` frames.
