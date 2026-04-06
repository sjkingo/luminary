#pragma once

/* Syscall numbers (passed in EAX via int 0x80).
 *
 * Numbers mirror the Linux i386 ABI where the semantics match closely enough
 * to be useful. Luminary-specific syscalls with no clean Linux equivalent are
 * assigned numbers in the 200+ range.
 */

#define SYS_NOP         0   /* no-op; returns 0 */
#define SYS_EXIT_TASK   1   /* exit(code) — terminate calling task */
#define SYS_FORK        2   /* fork() — clone task; child gets 0 */
#define SYS_READ        3   /* read(fd, buf, len) — blocking read */
#define SYS_WRITE       4   /* write(fd, buf, len) */
#define SYS_OPEN        5   /* open(path, flags) -> fd */
#define SYS_CLOSE       6   /* close(fd) */
#define SYS_WAITPID     7   /* waitpid(pid, &status, flags) — wait for child */
#define SYS_UNLINK      10  /* unlink(path) — remove file */
#define SYS_EXECVE      11  /* execve(path, argv, envp) — replace process image; envp=NULL inherits */
#define SYS_CHDIR       12  /* chdir(path) — change working directory */
#define SYS_LSEEK       19  /* lseek(fd, offset, whence) -> new offset */
#define SYS_GETPID      20  /* getpid() -> current task PID */
#define SYS_MOUNT       21  /* mount(fstype, path, device) — mount filesystem */
#define SYS_UMOUNT      22  /* umount(path) — unmount filesystem */
#define SYS_KILL        37  /* kill(pid) — terminate task by PID */
#define SYS_RENAME      38  /* rename(old, new) */
#define SYS_MKDIR       39  /* mkdir(path) — create directory */
#define SYS_PIPE        42  /* pipe(fds[2]) — create pipe */
#define SYS_BRK         45  /* brk(addr) — set/query program break */
#define SYS_IOCTL       54  /* ioctl(fd, request, arg) — device control */
#define SYS_FCNTL       55  /* fcntl(fd, cmd, arg) — file descriptor control */
#define SYS_DUP2        63  /* dup2(oldfd, newfd) — duplicate fd */
#define SYS_GETPPID     64  /* getppid() -> parent PID */
#define SYS_READDIR     89  /* readdir(fd, dirent*) -> 1/0/-1 */
#define SYS_STAT        106 /* stat(path, stat*) -> 0/-1 */
#define SYS_FSTAT       108 /* fstat(fd, stat*) -> 0/-1 */
#define SYS_GETCWD      183 /* getcwd(buf, len) — copy cwd into buf */

/* Luminary-specific syscalls (200+) */
#define SYS_YIELD       200 /* yield() — hlt and let scheduler run */
#define SYS_READ_NB     201 /* read_nb(fd, buf, len) — non-blocking read; 0 if empty */
#define SYS_TASK_DONE   202 /* task_done(pid) -> 1 if pid gone, 0 if still running */
