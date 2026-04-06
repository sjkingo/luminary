# Luminary OS — Syscall Interface

Syscalls are invoked via `int 0x80`. Number in EAX, up to three register args in EBX/ECX/EDX. Return value in EAX.

Userspace macros in `userland/syscall.h`: `syscall0(n)` through `syscall3(n, a, b, c)`.

The full list of syscall numbers is in `src/kernel/syscall_numbers.h`, included by both the kernel and userland. Numbers follow the Linux i386 ABI where semantics match closely. Syscalls with no clean Linux equivalent are assigned numbers in the 200+ range.

## Notes on non-obvious syscalls

**SYS_READ / SYS_WRITE**: dispatch depends on node flags. Pure chardevs (`VFS_CHARDEV` only) always I/O at offset 0. Nodes with `VFS_FILE` set (regular files, pipes, ext2 nodes) track position via the fd offset. SYS_READ on stdin blocks while the GUI owns the keyboard.

**SYS_EXECVE (11)**: EDX=NULL inherits the existing task environment unchanged. Pass a NULL-terminated `"NAME=VAL"` array to replace it. Userland wrappers: `execv(path, argv)` (inherits env) and `execve(path, argv, envp)`.

**SYS_MOUNT (21)**: EDX is an optional device path (e.g. `/dev/hda1`); the `/dev/` prefix is stripped before resolving. Pass 0 for memory-only filesystems. Registered drivers: `tmpfs`, `initrd`, `ext2`. `devfs` is kernel-internal and cannot be mounted from userspace.

**SYS_BRK (45)**: EBX=0 or EBX≤current break returns the current break without mapping anything. Initial break set by `elf_load` to the page-aligned end of the highest PT_LOAD segment. Userland wrappers: `brk(addr)` and `sbrk(increment)`.

**SYS_READ_NB (201)**: returns 0 immediately if no data is available. Equivalent to `O_NONBLOCK` on the read path. Used by the shell's Ctrl+C wait loop.

**SYS_TASK_DONE (202)**: non-blocking pid liveness check. Returns 1 if the pid is no longer in the scheduler queue.

## Device Nodes

Subsystem-specific operations are exposed as device nodes under `/dev`. Programs open the device and use `SYS_IOCTL` with request codes defined in the relevant userland header. Adding new device operations never requires a new syscall number.

### /dev/x — GUI / window manager

Open with `O_RDWR`. Read returns `struct x_mouse_state` (12 bytes: x, y, buttons). Request codes and structs in `userland/x.h`.

| Request | Arg struct | Description |
|---------|------------|-------------|
| X_WIN_CREATE (1) | `struct x_win_create` | Create window; returns id or -1 |
| X_WIN_DESTROY (2) | `struct x_win_destroy` | Destroy window by id |
| X_WIN_FILL_RECT (3) | `struct x_win_rect` | Fill rectangle in backbuffer |
| X_WIN_DRAW_RECT (4) | `struct x_win_rect` | Draw 1px outline rectangle |
| X_WIN_DRAW_TEXT (5) | `struct x_win_text` | Draw text into backbuffer |
| X_WIN_FLIP (6) | `struct x_win_flip` | Commit backbuffer to screen |
| X_WIN_POLL_EVENT (7) | `struct x_win_poll_event` | Non-blocking event poll; returns 1 if event filled |
| X_WIN_GET_SIZE (8) | `struct x_win_get_size` | Get client area dimensions |
| X_SET_BG (9) | `struct x_set_bg` | Set desktop background from ARGB pixel buffer |
| X_SET_DESKTOP_COLOR (10) | `struct x_set_desktop_color` | Set desktop fill colour (r, g, b in 0–255) |

### /dev/sys — system control and information

Open with `O_RDWR`. Request codes and structs in `userland/sys_dev.h`.

| Request | Arg | Description |
|---------|-----|-------------|
| SYS_CTL_HALT (1) | NULL | Halt the machine |
| SYS_CTL_REBOOT (2) | NULL | Reboot the machine |
| SYS_CTL_UPTIME (3) | `uint32_t *` | Fill with uptime in milliseconds |
| SYS_CTL_PS (4) | `struct sys_ctl_ps *` | Format process list into caller buffer |
| SYS_CTL_MOUNTS (5) | `struct sys_ctl_mounts *` | Format mount table into caller buffer |
| SYS_CTL_GUI_ACTIVE (6) | `uint32_t *` | 1 if GUI compositor owns the keyboard, 0 otherwise |

### /dev/env — per-task environment variables

Open with `O_RDONLY`. All operations act on the calling task's environ table. Request codes and struct in `userland/env_dev.h`.

| Request | Arg | Description |
|---------|-----|-------------|
| ENV_GET (1) | `struct env_op *` | Look up by `op->name`; fills `op->val`. Returns -1 if not found |
| ENV_SET (2) | `struct env_op *` | Set `op->name=op->val`; `op->overwrite=0` skips if already set. Returns -1 if table full |
| ENV_UNSET (3) | `struct env_op *` | Remove by `op->name`; no-op if not found |
| ENV_GET_IDX (4) | `struct env_op *` | Get `NAME=VAL` string at `op->index`. Returns -1 if out of range |

### /dev/fb0 — VBE framebuffer

Open with `O_RDWR`. The framebuffer is identity-mapped `PTE_USER`; write pixel data directly to `fb_addr` without mmap. Request codes and struct in `userland/fb_dev.h`.

| Request | Arg | Description |
|---------|-----|-------------|
| FBIOGET_INFO (1) | `struct fb_info *` | Fill with width, height, pitch, depth (bpp), fb_addr |
