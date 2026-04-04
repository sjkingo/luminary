# Luminary OS — Syscall Interface

Syscalls are invoked via `int 0x80`. Number in EAX, up to three register args in EBX/ECX/EDX. Additional args are pushed onto the user stack *before* `int $0x80` and read by the kernel from `frame->uesp + 0`, `+4`, `+8`. Return value in EAX.

Userspace macros (in `userland/syscall.h` and `userland/gui.h`):
- `_sc3(n, a, b, c)` — 3-register syscall
- `_sc3x1(n, a, b, c, x)` — 3 register + 1 stack arg
- `_sc3x3(n, a, b, c, x, y, z)` — 3 register + 3 stack args

## Syscall Table

| # | Name | Args | Returns | Description |
|---|------|------|---------|-------------|
| 0 | SYS_NOP | — | 0 | No-op |
| 1 | SYS_EXIT_TASK | — | — | Kill calling task (normal process exit) |
| 3 | SYS_READ | EBX=fd, ECX=buf, EDX=len | bytes read or -1 | Read from fd; blocks on chardev (stdin) until data available |
| 4 | SYS_WRITE | EBX=fd, ECX=buf, EDX=len | bytes written or -1 | Write to fd. Chardevs dispatched to write_op; writable regular files (O_CREAT/O_TRUNC opened) track offset; O_APPEND fds always write at end. |
| 5 | SYS_OPEN | EBX=path, ECX=flags | fd or -1 | Open VFS path. flags: O_RDONLY=0, O_WRONLY=1, O_CREAT=0x40, O_TRUNC=0x200, O_APPEND=0x400. O_CREAT\|O_TRUNC creates or truncates; resulting fd is writable (heap-backed, volatile). |
| 6 | SYS_CLOSE | EBX=fd | 0 or -1 | Close fd |
| 7 | SYS_PS | EBX=buf, ECX=len | bytes written or -1 | Format process list (PID/PRIO/NAME) into userland buffer |
| 8 | SYS_WIN_CREATE | EBX=x, ECX=y, EDX=w, [uesp+0]=h, [uesp+4]=title | id or -1 | Create window |
| 9 | SYS_WIN_DESTROY | EBX=id | 0 | Destroy window |
| 10 | SYS_WIN_FILL_RECT | EBX=id, ECX=x, EDX=y, [uesp+0]=w, [uesp+4]=h, [uesp+8]=color | 0 | Fill rect in window backbuffer |
| 11 | SYS_WIN_DRAW_TEXT | EBX=id, ECX=x, EDX=y, [uesp+0]=str, [uesp+4]=fg, [uesp+8]=bg | 0 | Draw text in window backbuffer |
| 12 | SYS_WIN_FLIP | EBX=id | 0 | Mark window dirty, wake compositor |
| 13 | SYS_WIN_POLL_EVENT | EBX=id, ECX=ev_ptr | 1 or 0 | Non-blocking event poll |
| 14 | SYS_MOUSE_GET | EBX=buf | 1 or 0 | Get mouse state {x, y, buttons} |
| 15 | SYS_WIN_DRAW_RECT | EBX=id, ECX=x, EDX=y, [uesp+0]=w, [uesp+4]=h, [uesp+8]=color | 0 | Draw rect outline in backbuffer |
| 16 | SYS_SPAWN | EBX=path | pid or -1 | Spawn ELF from VFS path as new task |
| 17 | SYS_KILL | EBX=pid | 0 or -1 | Kill task by PID |
| 18 | SYS_YIELD | — | 0 | Yield CPU (hlt) |
| 19 | SYS_WIN_GET_SIZE | EBX=id, ECX=&w, EDX=&h | 0 | Get client area dimensions |
| 20 | SYS_GETPID | — | pid | Current task PID |
| 21 | SYS_HALT | — | — | Shut down the machine |
| 22 | SYS_UPTIME | — | ms | System uptime in milliseconds |
| 24 | SYS_LSEEK | EBX=fd, ECX=offset, EDX=whence | new offset or -1 | Seek within fd |
| 25 | SYS_READDIR | EBX=fd, ECX=dirent_ptr | 1, 0, or -1 | Read next directory entry |
| 26 | SYS_STAT | EBX=path, ECX=stat_ptr | 0 or -1 | Stat a path |
| 27 | SYS_MOUNT | — | 0 | Print mount table |
| 28 | SYS_EXEC | EBX=path, ECX=argv | 0 or -1 | exec in-place: replace address space with ELF at path |
| 29 | SYS_FORK | — | child pid or 0 | Fork current task; child gets 0 |
| 30 | SYS_WAITPID | EBX=pid | pid or -1 | Block until child exits |
| 23 | SYS_READ_NB | EBX=fd, ECX=buf, EDX=len | bytes read or 0 | Non-blocking read: returns 0 immediately if no data available (pipe empty or no keyboard input) |
| 32 | SYS_PIPE | EBX=int[2] ptr | 0 or -1 | Create pipe; fills fds[0]=read end, fds[1]=write end |
| 33 | SYS_DUP2 | EBX=oldfd, ECX=newfd | newfd or -1 | Duplicate oldfd onto newfd; closes newfd first if open |
| 34 | SYS_TASK_DONE | EBX=pid | 1 or 0 | Non-blocking check: returns 1 if pid is no longer in the scheduler queue, 0 if still running |

## Notes

- Use **SYS_EXIT_TASK (1)** for normal task exit.
- **SYS_EXEC (28)** replaces the calling task's address space in-place. On failure, the task continues with its original image.
- **SYS_SPAWN (16)** creates a new independent task (no parent relationship). For fork+exec with wait, use SYS_FORK + SYS_EXEC + SYS_WAITPID.
- **SYS_READ (3)** routes through the fd table. For fd 0 (stdin chardev), it blocks until keyboard input is available and yields without consuming while the keyboard is owned by the GUI (`kbd_is_owned()`).
- **SYS_WRITE (4)** routes through the fd table. For fd 1/2 (stdout/stderr chardevs), it writes to the framebuffer console. For writable regular file fds (opened with O_CREAT or O_TRUNC), it writes to the heap buffer and advances the offset. Read-only (initrd-backed) fds return -1.
- **SYS_PS (7)** formats the process list as "PID  PRIO  NAME\n" entries into a userland-supplied buffer. Returns bytes written (not counting the null terminator), or -1 on error.
- **SYS_PIPE (32)** allocates a 4KB ring buffer shared between two chardev VFS nodes. Up to 16 concurrent pipes. The read end blocks when empty (until data arrives or write end closes). The write end blocks when full (until space is available or read end closes).
- **SYS_DUP2 (33)** is the standard mechanism for I/O redirection: `dup2(pipe_fds[0], 0)` redirects stdin to a pipe read end. Inherited across `fork()`; preserved across `exec()`.
- **SYS_READ_NB (23)** sets the `pipe_nonblock` flag, calls the normal VFS read path, then clears the flag. Works on any fd including fd 0 (stdin chardev). Used by the shell's Ctrl+C wait loop and by `term.elf` to drain pipe output without blocking.
- **SYS_TASK_DONE (34)** walks the scheduler queue and returns 1 if the pid is absent. Used together with `SYS_READ_NB` to implement an interruptible wait: poll `task_done(pid)` and `read_nb(0, &c, 1)` in a yield loop rather than blocking in `waitpid()`.
- Extra stack args are at `[uesp+0]`, `[uesp+4]`, `[uesp+8]` — note offset 0, not +4, because they are pushed before `int $0x80` and ESP points to the first pushed value.
