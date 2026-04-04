#pragma once

#include "cpu/traps.h"

/* Syscall numbers (passed in EAX) */
#define SYS_NOP         0   /* do nothing, return 0 */
#define SYS_EXIT_TASK   1   /* exit_task() - kill calling task (normal exit) */
/* 2 = unused */
#define SYS_READ        3   /* read(fd, buf, len) - read from fd; blocks on chardev stdin */
#define SYS_WRITE       4   /* write(fd, buf, len) - write to fd (chardev only) */
#define SYS_OPEN        5   /* open(path, flags) -> fd or -1; flags: O_RDONLY/O_WRONLY/O_CREAT/O_TRUNC/O_APPEND */
#define SYS_CLOSE       6   /* close(fd) -> 0 or -1; notifies pipe on pipe fds */
#define SYS_PS          7   /* ps(buf, len) - format process list into buf, returns bytes written */

/* GUI / window manager syscalls
 * Calling convention: EAX=syscall, EBX=arg0, ECX=arg1, EDX=arg2
 * Additional args read from user stack at [uesp+0], [uesp+4], [uesp+8], ...
 */
#define SYS_WIN_CREATE      8   /* (x, y, w, h) + title ptr at uesp+0 -> id */
#define SYS_WIN_DESTROY     9   /* (id) */
#define SYS_WIN_FILL_RECT   10  /* (id, x, y) + w,h,color at uesp+0..8 */
#define SYS_WIN_DRAW_TEXT   11  /* (id, x, y) + str,fgcolor,bgcolor at uesp+0..8 */
#define SYS_WIN_FLIP        12  /* (id) */
#define SYS_WIN_POLL_EVENT  13  /* (id, event_buf_ptr) -> 0 or 1 */
#define SYS_MOUSE_GET       14  /* (mouse_state_buf_ptr) -> 1 or 0 */
#define SYS_WIN_DRAW_RECT   15  /* (id, x, y) + w,h,color at uesp+0..8 */
#define SYS_KILL            17  /* kill(pid) -> 0 on success, -1 if not found */
#define SYS_YIELD           18  /* yield() - hlt and let scheduler run */
#define SYS_WIN_GET_SIZE    19  /* (id, &w, &h) -> client w/h in pixels */
#define SYS_GETPID          20  /* getpid() - return current task PID */
#define SYS_HALT            21  /* halt() - shut down the machine */
#define SYS_UPTIME          22  /* uptime() - return uptime in ms */

/* VFS / I/O syscalls */
#define SYS_READ_NB         23  /* read_nb(fd, buf, len) -> bytes or 0 if empty (non-blocking) */
#define SYS_LSEEK           24  /* lseek(fd, offset, whence) -> new offset or -1; no-op (0) on chardevs */
#define SYS_READDIR         25  /* readdir(fd, dirent_ptr) -> 1 if entry, 0 if done, -1 err */
#define SYS_STAT            26  /* stat(path, stat_ptr) -> 0 or -1 */
#define SYS_MOUNT           27  /* mount() -> prints mount info, returns 0 */
#define SYS_EXEC            28  /* exec(path, argv) - replace address space in-place; fds preserved */
#define SYS_FORK            29  /* fork() -> child PID in parent, 0 in child; fds inherited */
#define SYS_WAITPID         30  /* waitpid(pid, &status, flags) -> pid on exit, -1 on error; flags: WNOHANG=1 */
#define SYS_PIPE            32  /* pipe(int fds[2]) -> 0 or -1; fds[0]=read end, fds[1]=write end */
#define SYS_DUP2            33  /* dup2(oldfd, newfd) -> newfd or -1; closes newfd first if open */
#define SYS_TASK_DONE       34  /* task_done(pid) -> 1 if pid no longer exists, 0 if still running */
#define SYS_CHDIR           35  /* chdir(path) -> 0 or -1 */
#define SYS_GETCWD          36  /* getcwd(buf, len) -> 0 or -1 */
#define SYS_GETPPID         37  /* getppid() -> parent PID, 0 if no parent */
#define SYS_MKDIR           38  /* mkdir(path) -> 0 or -1 */
#define SYS_UNLINK          39  /* unlink(path) -> 0 or -1 */

#define SYS_MAX     39

/* Handle a syscall. Called from trap_handler when trapno == SYSCALL_VECTOR. */
void syscall_handler(struct trap_frame *frame);
