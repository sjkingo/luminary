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
| 1 | SYS_WRITE | EBX=buf, ECX=len | len | Write bytes to kernel console |
| 2 | SYS_EXIT | — | — | Halt with debug dump (legacy) |
| 3 | SYS_READ | EBX=buf, ECX=len | bytes read | Blocking keyboard read; yields without consuming while GUI windows open |
| 4 | SYS_UPTIME | — | ms | System uptime in milliseconds |
| 5 | SYS_GETPID | — | pid | Current task PID |
| 6 | SYS_HALT | — | — | Shut down the machine |
| 7 | SYS_PS | — | 0 | Print process list to console |
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
| 20 | SYS_READ_NB | EBX=buf, ECX=len | bytes read | Non-blocking keyboard read |
| 21 | SYS_OPEN | EBX=path | fd or -1 | Open VFS path |
| 22 | SYS_CLOSE | EBX=fd | 0 or -1 | Close fd |
| 23 | SYS_READ_FD | EBX=fd, ECX=buf, EDX=len | bytes or -1 | Read from fd |
| 24 | SYS_LSEEK | EBX=fd, ECX=offset, EDX=whence | new offset or -1 | Seek within fd |
| 25 | SYS_READDIR | EBX=fd, ECX=dirent_ptr | 1, 0, or -1 | Read next directory entry |
| 26 | SYS_STAT | EBX=path, ECX=stat_ptr | 0 or -1 | Stat a path |
| 27 | SYS_MOUNT | — | 0 | Print mount table |
| 28 | SYS_EXEC | EBX=path, ECX=argv | 0 or -1 | exec in-place: replace address space with ELF at path |
| 29 | SYS_FORK | — | child pid or 0 | Fork current task; child gets 0 |
| 30 | SYS_WAITPID | EBX=pid | pid or -1 | Block until child exits |
| 31 | SYS_EXIT_TASK | — | — | Kill calling task (normal process exit) |

## Notes

- **SYS_EXIT (2)** is the legacy halt-with-dump. Use **SYS_EXIT_TASK (31)** for normal task exit.
- **SYS_EXEC (28)** replaces the calling task's address space in-place. On failure, the task continues with its original image.
- **SYS_SPAWN (16)** creates a new independent task (no parent relationship). For fork+exec with wait, use SYS_FORK + SYS_EXEC + SYS_WAITPID.
- **SYS_READ (3)** blocks and yields without consuming keyboard input while `gui_has_windows()` is true — all keyboard input routes to the GUI compositor in that case.
- Extra stack args are at `[uesp+0]`, `[uesp+4]`, `[uesp+8]` — note offset 0, not +4, because they are pushed before `int $0x80` and ESP points to the first pushed value.
