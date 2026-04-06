# Luminary OS — Syscall Interface

Syscalls are invoked via `int 0x80`. Number in EAX, up to three register args in EBX/ECX/EDX. Return value in EAX.

Userspace macros (in `userland/syscall.h`):
- `syscall0(n)` through `syscall3(n, a, b, c)` — 0–3 register args

## Syscall Table

| # | Name | Args | Returns | Description |
|---|------|------|---------|-------------|
| 0 | SYS_NOP | — | 0 | No-op |
| 1 | SYS_EXIT_TASK | EBX=exit_code | — | Kill calling task (normal process exit) |
| 3 | SYS_READ | EBX=fd, ECX=buf, EDX=len | bytes read or -1 | Read from fd. Pure chardevs (VFS_CHARDEV only): always reads at offset 0, no seeking. Files and file+chardev nodes (VFS_FILE set): offset-tracked, advances fd position. Blocks on stdin when keyboard is owned by GUI. |
| 4 | SYS_WRITE | EBX=fd, ECX=buf, EDX=len | bytes written or -1 | Write to fd. Pure chardevs write at offset 0. Files and file+chardev nodes (e.g. ext2 files) write at fd position and advance it. |
| 5 | SYS_OPEN | EBX=path, ECX=flags | fd or -1 | Open VFS path. flags: O_RDONLY=0, O_WRONLY=1, O_RDWR=2, O_CREAT=0x40, O_TRUNC=0x200, O_APPEND=0x400 |
| 6 | SYS_CLOSE | EBX=fd | 0 or -1 | Close fd |
| 17 | SYS_KILL | EBX=pid | 0 or -1 | Kill task by PID |
| 18 | SYS_YIELD | — | 0 | Yield CPU (hlt) |
| 20 | SYS_GETPID | — | pid | Current task PID |
| 23 | SYS_READ_NB | EBX=fd, ECX=buf, EDX=len | bytes read or 0 | Non-blocking read; returns 0 immediately if no data |
| 24 | SYS_LSEEK | EBX=fd, ECX=offset, EDX=whence | new offset or -1 | Seek within fd |
| 25 | SYS_READDIR | EBX=fd, ECX=dirent_ptr | 1, 0, or -1 | Read next directory entry |
| 26 | SYS_STAT | EBX=path, ECX=stat_ptr | 0 or -1 | Stat a path |
| 28 | SYS_EXEC | EBX=path, ECX=argv | 0 or -1 | exec in-place: replace address space with ELF at path |
| 29 | SYS_FORK | — | child pid or 0 | Fork current task; child gets 0 |
| 30 | SYS_WAITPID | EBX=pid, ECX=&status (or 0), EDX=flags | pid or -1 | Block until child exits. flags: WNOHANG=1 |
| 32 | SYS_PIPE | EBX=int[2] ptr | 0 or -1 | Create pipe; fills fds[0]=read end, fds[1]=write end |
| 33 | SYS_DUP2 | EBX=oldfd, ECX=newfd | newfd or -1 | Duplicate oldfd onto newfd |
| 34 | SYS_TASK_DONE | EBX=pid | 1 or 0 | Non-blocking check: 1 if pid is no longer in scheduler |
| 35 | SYS_CHDIR | EBX=path | 0 or -1 | Change current working directory |
| 36 | SYS_GETCWD | EBX=buf, ECX=len | 0 or -1 | Copy cwd string into buf |
| 37 | SYS_GETPPID | — | ppid | Parent PID; 0 if no parent |
| 38 | SYS_MKDIR | EBX=path | 0 or -1 | Create directory; parent must exist |
| 39 | SYS_UNLINK | EBX=path | 0 or -1 | Remove regular file |
| 43 | SYS_IOCTL | EBX=fd, ECX=request, EDX=arg | int32 or -1 | Device control: dispatch to node's control_op |
| 44 | SYS_FCNTL | EBX=fd, ECX=cmd, EDX=arg | int or -1 | File control: F_GETFL returns flags; F_SETFL sets O_APPEND/O_NONBLOCK |
| 46 | SYS_MOUNT | EBX=fstype, ECX=path, EDX=device (or 0) | 0 or -1 | Mount registered filesystem at path. EDX is a device path (e.g. /dev/hda1) for block-backed filesystems; 0 for memory-only filesystems (tmpfs). |
| 47 | SYS_UMOUNT | EBX=path | 0 or -1 | Unmount filesystem at path; fails if nested mounts exist underneath |
| 48 | SYS_FSTAT | EBX=fd, ECX=stat_ptr | 0 or -1 | Stat an open file descriptor |
| 49 | SYS_RENAME | EBX=old_path, ECX=new_path | 0 or -1 | Rename or move a file or directory |
| 50 | SYS_BRK | EBX=new_brk | new_brk or cur_brk | Set program break; returns current break if EBX=0 or EBX≤current |
| 51 | SYS_EXECVE | EBX=path, ECX=argv[], EDX=envp[] | 0 or -1 | exec in-place with explicit environment; replaces task environ with envp[] after successful load |

## Device Nodes

Rather than adding new syscalls, subsystem-specific operations are exposed as device nodes under `/dev`. Programs open the device and use `SYS_IOCTL` with request codes defined in the relevant userland header.

### /dev/x — GUI / window manager

Open with `O_RDWR`. Read returns `struct x_mouse_state` (12 bytes: x, y, buttons).

Request codes and argument structs are defined in `userland/x.h`.

| Request | Arg struct | Description |
|---------|------------|-------------|
| X_WIN_CREATE (1) | `struct x_win_create` | Create window; returns id or -1 |
| X_WIN_DESTROY (2) | `struct x_win_destroy` | Destroy window by id |
| X_WIN_FILL_RECT (3) | `struct x_win_rect` | Fill rectangle in backbuffer |
| X_WIN_DRAW_RECT (4) | `struct x_win_rect` | Draw 1px outline rectangle |
| X_WIN_DRAW_TEXT (5) | `struct x_win_text` | Draw text into backbuffer |
| X_WIN_FLIP (6) | `struct x_win_flip` | Commit backbuffer to screen |
| X_WIN_POLL_EVENT (7) | `struct x_win_poll_event` | Non-blocking event poll; returns 1 if event filled |
| X_WIN_GET_SIZE (8) | `struct x_win_get_size` | Get client area dimensions (filled in w, h fields) |
| X_SET_BG (9) | `struct x_set_bg` | Set desktop background from ARGB pixel buffer |
| X_SET_DESKTOP_COLOR (10) | `struct x_set_desktop_color` | Set desktop fill colour (r, g, b in 0–255) |

### /dev/sys — system control and information

Open with `O_RDWR`. No read op.

Request codes and argument structs are defined in `userland/sys_dev.h`.

| Request | Arg | Description |
|---------|-----|-------------|
| SYS_CTL_HALT (1) | NULL | Halt the machine |
| SYS_CTL_REBOOT (2) | NULL | Reboot the machine |
| SYS_CTL_UPTIME (3) | `uint32_t *` | Fill with uptime in milliseconds |
| SYS_CTL_PS (4) | `struct sys_ctl_ps *` | Format process list into caller buffer |
| SYS_CTL_MOUNTS (5) | `struct sys_ctl_mounts *` | Format mount table into caller buffer |
| SYS_CTL_GUI_ACTIVE (6) | `uint32_t *` | Set to 1 if the GUI compositor currently owns the keyboard, 0 otherwise |

### /dev/env — per-task environment variables

Open with `O_RDONLY`. No read/write op. All operations act on the calling task's environ table (`running_task->environ[32][128]`).

The environ table is stored inline in `struct task`. Fork copies it automatically. `SYS_EXEC` preserves it; `SYS_EXECVE` replaces it with the supplied `envp[]`.

Request codes and argument struct are defined in `userland/env_dev.h`.

| Request | Arg | Description |
|---------|-----|-------------|
| ENV_GET (1) | `struct env_op *` | Look up variable by `op->name`; fills `op->val` with value. Returns -1 if not found |
| ENV_SET (2) | `struct env_op *` | Set `op->name=op->val`; `op->overwrite=0` skips if already set. Returns -1 if table full |
| ENV_UNSET (3) | `struct env_op *` | Remove variable by `op->name`; no-op if not found |
| ENV_GET_IDX (4) | `struct env_op *` | Get `NAME=VAL` string at `op->index`; fills `op->val`. Returns -1 if index out of range |

### /dev/fb0 — VBE framebuffer

Open with `O_RDWR`. No read op. The VBE framebuffer is identity-mapped with `PTE_USER`; userland can write pixel data directly to `fb_addr` without any mmap syscall.

Request codes and the `fb_info` struct are defined in `userland/fb_dev.h`.

| Request | Arg | Description |
|---------|-----|-------------|
| FBIOGET_INFO (1) | `struct fb_info *` | Fill with framebuffer geometry: width, height, pitch, depth (bpp), fb_addr |

## Notes

- **SYS_IOCTL (43)** routes `(fd, request, arg)` to the `control_op` of the node backing `fd`. Returns -1 if the node has no `control_op`.
- **Adding new device operations** never requires a new syscall number — register a chardev under `/dev` via `vfs_register_dev()` and implement a `control_op`.
- **SYS_READ_NB (23)** is non-blocking: returns 0 immediately if no data. Used by the shell's Ctrl+C wait loop.
- **SYS_TASK_DONE (34)** walks the scheduler queue; returns 1 if pid is absent.
- **SYS_FCNTL (44)** supports `F_GETFL`/`F_SETFL` with `O_APPEND` (0x400) and `O_NONBLOCK` (0x800). `O_NONBLOCK` on an fd makes `SYS_READ` behave like `SYS_READ_NB` for that fd.
- **SYS_MOUNT (46)** calls `vfs_do_mount(path, fstype, device)` in the kernel. The filesystem driver must have been registered with `vfs_fs_register()`. EDX is an optional device path string (e.g. `"/dev/hda1"`); the kernel strips the `/dev/` prefix and resolves it via `blkdev_find`. Pass 0/NULL in EDX for memory-only filesystems. Currently registered drivers: `tmpfs`, `initrd`, `ext2`. Valid userland invocations: `mount tmpfs /tmp`; `mount initrd /foo` (initrd boot path only — requires `init_initrd` to have run); `mount ext2 /dev/hda1 /mnt` or `mount ext2 hda1 /mnt` (`/dev/` prefix is optional). `devfs` is kernel-internal only and cannot be mounted from userspace.
- **SYS_UMOUNT (47)** calls `vfs_do_umount(path)`. The kernel calls `fs_ops->umount()` on the mounted root; all nodes in the subtree are freed. Fails if the fs driver rejects the unmount (e.g. busy fds).
- Extra stack args convention is retired — all new operations use ioctl structs.
- **SYS_FSTAT (48)** is the fd-based variant of SYS_STAT. Fills the same `struct vfs_stat` (size, type, inode).
- **SYS_RENAME (49)** follows Linux `rename(2)` semantics: atomically replaces an existing file target; replaces an empty directory target when both old and new are directories; refuses to move a directory into itself.
- **SYS_BRK (50)** follows Linux `brk(2)` semantics. EBX=0 or EBX≤current break returns the current break without mapping anything. EBX>current maps new pages (PTE_PRESENT|PTE_WRITE|PTE_USER) between old and new break in the current task's address space. Returns the new break on success, or the old break on failure (OOM or stack collision). The initial break is set by `elf_load` to the page-aligned end of the highest PT_LOAD segment. Userland wrappers: `brk(addr)` (raw) and `sbrk(increment)` (POSIX-style) in `userland/syscall.h`.
- **SYS_EXECVE (51)** is like `SYS_EXEC` but accepts an `envp[]` (NULL-terminated array of `"NAME=VAL"` strings) as EDX. The envp strings are copied to kernel scratch before the old address space is destroyed. On success, the task's `environ` table is replaced with the supplied entries. Pass EDX=NULL to inherit the existing environment (behaves identically to `SYS_EXEC`). Userland wrapper: `execve(path, argv, envp)` in `userland/syscall.h`.
