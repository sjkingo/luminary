# Luminary OS — Architecture

Luminary is a modular x86 (IA-32) kernel designed with real-time scheduling in mind. Written by Sam Kingston, spiritual successor to Ulysses. Written in C (gnu99) and x86 assembly (GAS syntax).

## Target Platform

- Architecture: IA-32 (32-bit x86 protected mode)
- Boot: GRUB2 Multiboot → `src/boot/boot.S` → `kernel_main()`
- Kernel load address: 0x00100000 (1MB), defined in `src/linker.ld`
- Kernel stack: 16KB
- VBE: Requests 1024x768x32bpp via Multiboot flags
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
- Active build defines: `-DUSE_SERIAL`
- Available defines: `-DDEBUG` (heap tracking), `-DTURTLE` (1Hz scheduling), `-DUSE_SERIAL`
- `src/cpu/mkvectors.py` generates `vectors.S` (256 interrupt stubs)
- `tools/make_version_h.sh` runs `git describe` to generate `version.h`

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

The kernel is identity-mapped: virtual == physical for the first 8MB (kernel, heap, VGA, low memory) and the VBE framebuffer region. Paging is enabled in `init_vmm()`.

Each user task gets its own page directory. `vmm_create_page_dir()` allocates a new directory that inherits all kernel PDEs by reference. User space spans `0x01000000–0xC0000000`; the user stack lives at `0xBFFFF000`.

`vmm_alloc_pages(n)` provides a kernel virtual allocator: it maps N physical frames (which need not be contiguous) to a contiguous virtual range starting at `0xC0000000`. Page tables for both `0–128MB` and `0xC0000000–0xC3FFFFFF` are pre-allocated in `init_vmm()` before paging is enabled to avoid the chicken-and-egg problem of allocating a PT frame that isn't yet mapped.

## Scheduling

Priority-based preemptive scheduler with dynamic aging. Tasks have a static priority (`prio_s`, 1–10) and a dynamic priority (`prio_d`) that decrements each time the task runs. When all non-idle tasks reach `prio_d=0`, priorities reset. Tie-break: the most recently run task. The scheduler runs on every timer tick (1000Hz). The idle task (PID 0) runs at priority -1.

## Process Model

Tasks are created with per-task 4KB kernel stacks and synthetic trap frames for initial context switching via `trapret`. Context switches swap CR3 (page directory) and update TSS esp0.

Init (PID 1) is loaded from the initrd and respawned automatically if killed — analogous to Unix PID 1. `task_fork()` does a full address space copy (no CoW). `task_exec()` replaces the calling task's address space in-place.

## Trap Handling

All 256 interrupt vectors go through `alltraps` in `traps.S`, which saves the full register state (with magic cookie `0xc0ffee` for stack validation) and calls `trap_handler()` in C. On return, `alltraps` checks `prev_task` to determine if a context switch is needed, then does `iret`. Syscalls use vector `0x80` with a ring 3 gate.

## Filesystem

CPIO newc initrd, parsed by `initrd.c` and mounted read-only at `/` on boot. A VFS layer (`vfs.c`) provides mount table, path resolution, and open/read/readdir/stat/close. Per-task fd tables are stored inline in `struct task`.

## GUI

Compositor and window manager running as a kernel task at priority 9. Three-buffer design: per-window backbuffers (written by apps), a clean scene buffer (`back`), and the hardware framebuffer (`fb_hw`). See `docs/gui.md`.

## Userland

Init spawns `/bin/sh` via fork+exec and respawns it if it exits. The shell supports VFS commands, path-based exec, and standard builtins. A GUI demo app (`/bin/gui`) opens 4 windows including a console. All userspace programs are ELF32 binaries built with the same `i686-elf-gcc` toolchain and a freestanding libc.

## Important Constants & Addresses

| Constant | Value | Location |
|----------|-------|----------|
| Kernel load addr | 0x00100000 | linker.ld |
| VGA framebuffer | 0xB8000 | vga.h |
| PIT frequency | 1000 Hz (1 Hz if TURTLE) | cpu.c |
| IRQ base vector | 32 | x86.h |
| Trap magic | 0xc0ffee | x86.h |
| Heap block size | 1024 bytes | heap.h |
| Heap max blocks | 1024 (1MB total) | heap.h |
| Idle task PID | 0 | task.h |
| Init task PID | 1 | task.h |
| Max priority | 10 | sched.h |
| Page size | 4096 bytes | pmm.h |
| Max physical frames | 32768 (128MB) | pmm.c |
| User space start | 0x01000000 | vmm.h |
| User space end | 0xC0000000 | vmm.h |
| User stack top | 0xBFFFF000 | vmm.h |
| Kernel virtual alloc base | 0xC0000000 | vmm.c |
| Syscall vector | 0x80 | syscall.h |
| GUI max windows | 16 | gui.h |
| Compositor priority | 9 | gui.c |
