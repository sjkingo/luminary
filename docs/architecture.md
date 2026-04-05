# Luminary OS — Architecture

Luminary is a modular x86 (IA-32) kernel designed with real-time scheduling in mind. Written by Sam Kingston, spiritual successor to Ulysses. Written in C (gnu99) and x86 assembly (GAS syntax).

## Target Platform

- Architecture: IA-32 (32-bit x86 protected mode)
- Boot: GRUB2 Multiboot → `src/boot/boot.S` → `kernel_main()`
- Kernel load address: 0x00100000 (1MB), defined in `src/linker.ld`
- Kernel stack: 16KB
- VBE: Requests 1280x960x32bpp via Multiboot flags
- GDT: 6 entries (null, kernel code 0x9A, kernel data 0x92, user code 0xFA, user data 0xF2, TSS)
- User mode: Ring 3 via TSS and `int 0x80` syscall gate

## Directory Structure

```
src/
  boot/           # Multiboot entry, multiboot header definitions
  kernel/         # Core: kernel_main, scheduler, tasks, heap, printk, ELF loader, syscalls, GUI
  cpu/            # GDT, IDT, TSS, PIC, trap handling, vector generation
  drivers/        # VGA text, VBE framebuffer, serial, keyboard, mouse, RTL8139 NIC
  lib/            # Freestanding string library
  pci/            # PCI bus enumeration and driver registry
  fonts/          # 8x16 bitmap console font
  linker.ld
  Makefile
userland/         # Userspace programs (init, sh, gui), libc, crt0
tools/            # make_version_h.sh, gdbinitrc
grub2/            # GRUB2 config and ISO build
docs/
```

## Build System

- Compiler: `i686-elf-gcc -std=gnu99 -ffreestanding -g`
- Assembler: `i686-elf-gcc` (GAS syntax)
- Active build defines: `-DDEBUG`
- Available defines: `-DDEBUG` (heap tracking, debug serial output via `DBGK()`)
- `src/cpu/mkvectors.py` generates `vectors.S` (256 interrupt stubs)
- `tools/make_version_h.sh` runs `git describe` to generate `version.h`
- Two-pass build for kernel symbol table: pass 1 links without `symtab_gen.o` to get stable addresses, `tools/gen_symtab.sh` runs `nm`+`addr2line` to emit `kernel/symtab_gen.c`, pass 2 links the final binary with symbol data included for stack traces

### Make Targets

| Target | Description |
|--------|-------------|
| `make` | Build kernel binary |
| `make qemu` | Direct kernel boot with QEMU (serial to /tmp/luminary.log) |
| `make qemucd` | Build GRUB2 ISO and boot from CD-ROM |
| `make console` | QEMU -nographic (requires USE_SERIAL) |
| `make debug` | QEMU with GDB stub (-s -S) |
| `make gdb` | Connect GDB to running QEMU |
| `make clean` | Clean build artifacts |

## Memory Model

The kernel identity-maps the first 16MB (virtual == physical), covering the kernel image, stack, heap, VGA, and the entirety of ZONE_LOW (which spans 1MB–16MB). Paging is enabled in `init_vmm()`.

All structural VMM frames — page directories, page tables — are allocated from ZONE_LOW, which means they are always within the identity-mapped region and directly accessible by physical address. User data frames are allocated from ZONE_ANY (above 16MB) and are transiently mapped/unmapped when `vmm_clone_page_dir` needs to copy them.

Each user task gets its own page directory. `vmm_create_page_dir()` allocates a new directory that inherits all kernel PDEs by reference. User space spans `0x01000000–0xC0000000`; the user stack lives at `0xBFFFF000`.

### PMM zones

The physical memory manager uses two allocation zones to prevent fragmentation from polluting contiguous-frame requirements:

- **ZONE_LOW** (`0x00100000–0x01000000`, ~15MB): reserved for physically contiguous allocations — the GUI back buffer, future DMA. Scanned high-to-low within the zone to preserve low addresses for runs. `pmm_alloc_contiguous(n)` allocates n contiguous frames from this zone.
- **ZONE_ANY** (`0x01000000` and above): general purpose — task stacks, page tables, ELF segments. First-fit, low-to-high. Falls back to ZONE_LOW for single-frame allocations if exhausted.

PMM is initialised from the Multiboot memory map (`mmap_addr`/`mmap_length`), not the `mem_upper` field. All frames start marked used; only `MEMORY_TYPE_RAM` regions are freed. This correctly handles sparse memory maps and removes the previous 128MB limit (bitmap is 128KB in BSS, supports up to 4GB).

### VMM kernel virtual allocator

`vmm_alloc_pages(n)` maps N physical frames (non-contiguous) to a contiguous virtual range starting at `0xC0000000`. Uses a 64-entry free-list so `vmm_free_pages()` reclaims virtual address space for reuse. Falls back to a bump allocator when the free-list has no suitable range.

Page tables for all managed physical memory (driven by `pmm_total_frames()`) and for `0xC0000000–0xCFFFFFFF` (256MB of kernel virtual space, 64 PDEs) are pre-allocated in `init_vmm()` before paging is enabled to avoid the chicken-and-egg problem of needing a mapped PT frame to map a PT frame.

## Scheduling

Priority-based preemptive scheduler with dynamic aging. Tasks have a static priority (`prio_s`, 1–10) and a dynamic priority (`prio_d`) that decrements each time the task runs. When all non-idle tasks reach `prio_d=0`, priorities reset. Tie-break: the most recently run task. The scheduler runs on every timer tick (1000Hz). The idle task (PID 0) runs at priority -1.

### CPU accounting

Each task has a `ticks` counter (lifetime) and `ticks_window` (current 1-second window). On every timer tick, `sched()` increments `total_ticks` and, if `running_task->blocking` is false, increments `running_task->ticks` and `running_task->ticks_window`. Every 1000 ticks (1 second), `cpu_pct = ticks_window / 10` is computed for each task and `ticks_window` is reset. Tasks in a blocking `hlt` loop set `blocking = true` around the `hlt` instruction so their wait time is not charged as CPU usage.

## Process Model

Tasks are created with per-task 16KB kernel stacks. Each stack is allocated by `kstack_alloc()` which maps `(TASK_STACK_SIZE/PAGE_SIZE) + 1` virtual pages and unmaps the first page as a guard page — stack overflow causes a page fault rather than silently corrupting adjacent data. Synthetic trap frames are built on the stack for initial context switching via `trapret`. Context switches swap CR3 (page directory) and update TSS esp0.

## Heap

`kmalloc`/`kfree` are backed by a slab allocator with 8 size classes: 32, 64, 128, 256, 512, 1024, 2048, 4096 bytes. Each class maintains up to 64 4KB PMM pages; within each page, slots are tracked by a 128-bit bitmap. Allocations larger than 4096 bytes go to an overflow list (up to 128 entries) backed by `vmm_alloc_pages`. With `-DDEBUG`, each alloc/free prints the call site and slab class to serial.

Init (PID 1) is loaded from the initrd and respawned automatically if killed — analogous to Unix PID 1. `task_fork()` clones the address space with copy-on-write: writable pages are marked read-only and shared; a page-fault on either side triggers a copy (`vmm_cow_fault`). `task_exec()` replaces the calling task's address space in-place. `waitpid(pid, &status, flags)` supports `WNOHANG` and propagates the child's exit code via `exit_status` in `struct task`.

`task_death_hook` is a function pointer in `task.h` called by `task_kill()` with the dying task's pid before its page directory is freed. Subsystems register here to clean up per-task resources without creating a compile-time dependency on `task.c`. The GUI subsystem registers `gui_destroy_windows_for_pid` here at `init_gui()` time.

## Trap Handling

All 256 interrupt vectors go through `alltraps` in `traps.S`, which saves the full register state (with magic cookie `0xc0ffee` for stack validation) and calls `trap_handler()` in C. On return, `alltraps` checks `prev_task` to determine if a context switch is needed, then does `iret`. Syscalls use vector `0x80` with a ring 3 gate.

Unhandled CPU exceptions call `dump_trap_frame()` which prints registers and walks the EBP chain for a kernel stack trace, resolving addresses to `function (file:line)` via the embedded symbol table. User-mode exceptions kill the offending task rather than panicking. A global consecutive-fault counter triggers a kernel panic if more than 5 faults fire without a clean exec or exit in between.

## Filesystem

CPIO newc initrd, parsed by `initrd.c` and mounted read-only at `/` on boot. A VFS layer (`vfs.c`) provides mount table, path resolution, and open/read/readdir/stat/close/creat/mkdir/unlink. Per-task fd tables are stored inline in `struct task`. `vfs_alloc_node()` uses a static pool of 512 nodes with a free-list (`sibling` pointer reused as link) so closed pipe nodes and unlinked file nodes are reclaimed and reused.

### Filesystem drivers and mount points

`struct fs_ops` defines a vtable with `mount` and `umount` callbacks. Drivers register with `vfs_fs_register(fstype, ops)` at init time. `vfs_do_mount(path, fstype)` resolves the path, looks up the driver, calls `fs_ops->mount(mountpoint)`, and records the entry in the mount table. `vfs_do_umount(path)` calls `fs_ops->umount(mountpoint)` which frees the subtree, then removes the table entry.

`vfs_lookup` follows `node->mounted_root` at each step: if a directory node has a filesystem mounted on it, traversal continues into `mounted_root` rather than the node's own children. This makes mounts transparent to all path-based syscalls.

Registered filesystem drivers:

| Driver | fstype | Description |
|--------|--------|-------------|
| `tmpfs` (`tmpfs.c`) | `"tmpfs"` | In-memory writable filesystem. `mount` allocates a fresh VFS_DIR root node; files use the standard heap-backed writable node mechanism. `umount` frees the entire subtree. |

On boot: `/` is initrd (read-only, registered via `vfs_mount`), `/dev` is devfs (registered via `vfs_mount`), `/tmp` is mounted as tmpfs via `vfs_do_mount`.

Userspace mounts arbitrary tmpfs directories with `mount tmpfs /path` (`SYS_MOUNT 46`) and unmounts with `umount /path` (`SYS_UMOUNT 47`).

## GUI

Compositor and window manager running as a kernel task at priority 9. Three-buffer design: per-window backbuffers (written by apps), a clean scene buffer (`back`), and the hardware framebuffer (`fb_hw`). See `docs/gui.md`.

## Userland

Init spawns `/bin/sh` via fork+exec and respawns it if it exits. The shell supports VFS commands, path-based exec, pipelines, I/O redirection, and background jobs.

Shell builtins: `help`, `echo`, `getpid`, `pwd`, `cd`, `exit`, `jobs`, `fg [n]`, `crash`.

Background jobs (`&`): a trailing `&` on any command line (including pipelines) forks all children as normal but skips the foreground wait loop. The job is stored in a static table (8 slots). Finished jobs are reaped and reported before each prompt. `fg [n]` brings job n (or the most recent) to the foreground and enters the normal Ctrl+C-interruptible wait loop. Ctrl+C at the prompt does not affect background jobs — only the current foreground pipeline is killed.

Userspace programs:
- `/bin/gui` — GUI demo app, opens 4 windows including a console
- `/bin/term` — GUI terminal emulator; forks `/bin/sh` and connects it via pipes, rendering output into a window and routing keypresses back to the shell's stdin
- `/bin/ps` — process list with tree view, per-task CPU% (1s rolling window), state, and elapsed time; summary line shows total CPU%, idle%, and uptime
- `/bin/uptime` — prints system uptime as `h:mm:ss`
- `/bin/busy` — infinite loop for CPU load testing

All userspace programs are ELF32 binaries built with the same `i686-elf-gcc` toolchain and a freestanding libc.

## Important Constants & Addresses

| Constant | Value | Location |
|----------|-------|----------|
| Kernel load addr | 0x00100000 | linker.ld |
| VGA framebuffer | 0xB8000 | vga.h |
| PIT frequency | 1000 Hz | cpu.c |
| IRQ base vector | 32 | x86.h |
| Trap magic | 0xc0ffee | x86.h |
| Slab size classes | 8 (32–4096 bytes) | heap.h |
| Max slabs per class | 64 pages | heap.c |
| Idle task PID | 0 | task.h |
| Init task PID | 1 | task.h |
| Max priority | 10 | sched.h |
| Page size | 4096 bytes | pmm.h |
| Max physical frames | 1M (4GB) | pmm.c |
| User space start | 0x01000000 | vmm.h |
| User space end | 0xC0000000 | vmm.h |
| User stack top | 0xBFFFF000 | vmm.h |
| Kernel virtual alloc base | 0xC0000000 | vmm.c |
| Syscall vector | 0x80 | syscall.h |
| GUI max windows | 16 | gui.h |
| Compositor priority | 9 | gui.c |
