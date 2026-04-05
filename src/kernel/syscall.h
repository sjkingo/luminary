#pragma once

#include "cpu/traps.h"

/* Syscall numbers (passed in EAX) */
#define SYS_NOP         0   /* do nothing, return 0 */
#define SYS_EXIT_TASK   1   /* exit_task() - kill calling task (normal exit) */
/* 2 = unused */
#define SYS_READ        3   /* read(fd, buf, len) - read from fd; blocks on chardev stdin */
#define SYS_WRITE       4   /* write(fd, buf, len) - write to fd */
#define SYS_OPEN        5   /* open(path, flags) -> fd or -1 */
#define SYS_CLOSE       6   /* close(fd) -> 0 or -1 */
/* 7–22 retired — use SYS_IOCTL on /dev/x or /dev/sys instead */
#define SYS_KILL        17  /* kill(pid) -> 0 on success, -1 if not found */
#define SYS_YIELD       18  /* yield() - hlt and let scheduler run */
#define SYS_GETPID      20  /* getpid() - return current task PID */

/* VFS / I/O syscalls */
#define SYS_READ_NB     23  /* read_nb(fd, buf, len) -> bytes or 0 if empty (non-blocking) */
#define SYS_LSEEK       24  /* lseek(fd, offset, whence) -> new offset or -1 */
#define SYS_READDIR     25  /* readdir(fd, dirent_ptr) -> 1 if entry, 0 if done, -1 err */
#define SYS_STAT        26  /* stat(path, stat_ptr) -> 0 or -1 */
#define SYS_EXEC        28  /* exec(path, argv) - replace address space in-place */
#define SYS_FORK        29  /* fork() -> child PID in parent, 0 in child */
#define SYS_WAITPID     30  /* waitpid(pid, &status, flags) -> pid on exit, -1 on error */
#define SYS_PIPE        32  /* pipe(int fds[2]) -> 0 or -1 */
#define SYS_DUP2        33  /* dup2(oldfd, newfd) -> newfd or -1 */
#define SYS_TASK_DONE   34  /* task_done(pid) -> 1 if pid no longer exists, 0 if still running */
#define SYS_CHDIR       35  /* chdir(path) -> 0 or -1 */
#define SYS_GETCWD      36  /* getcwd(buf, len) -> 0 or -1 */
#define SYS_GETPPID     37  /* getppid() -> parent PID, 0 if no parent */
#define SYS_MKDIR       38  /* mkdir(path) -> 0 or -1 */
#define SYS_UNLINK      39  /* unlink(path) -> 0 or -1 */
#define SYS_IOCTL       43  /* ioctl(fd, request, arg) -> int32 result */
#define SYS_FCNTL       44  /* fcntl(fd, cmd, arg) -> int or -1 */
#define SYS_SPAWN       45  /* spawn(path, argv) -> new pid or -1; no parent relationship */
#define SYS_MOUNT       46  /* mount(fstype, path) -> 0 or -1 */
#define SYS_UMOUNT      47  /* umount(path) -> 0 or -1 */

#define SYS_MAX         47

/* Handle a syscall. Called from trap_handler when trapno == SYSCALL_VECTOR. */
void syscall_handler(struct trap_frame *frame);
