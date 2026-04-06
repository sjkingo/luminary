#pragma once

/* User-space syscall stubs for Luminary OS.
 * Convention: syscall number in EAX, args in EBX, ECX, EDX.
 * Return value in EAX. */

#ifndef NULL
#define NULL ((void *)0)
#endif

#include "../src/kernel/syscall_numbers.h"

#define WNOHANG         1   /* waitpid flag: return -1 immediately if child hasn't exited */

/* VFS node type flags (must match kernel/vfs.h) */
#define VFS_FILE    0x01
#define VFS_DIR     0x02
#define VFS_CHARDEV 0x04

/* struct vfs_dirent (must match kernel/vfs.h layout) */
#define VFS_NAME_MAX 128
struct vfs_dirent {
    char         name[VFS_NAME_MAX];
    unsigned int inode;
    unsigned char type;
};

/* struct vfs_stat (must match kernel/vfs.h layout) */
struct vfs_stat {
    unsigned int  size;
    unsigned char type;
    unsigned int  inode;
};

/* SEEK constants */
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

/* open() flags — Linux i386 values */
#define O_RDONLY   0
#define O_WRONLY   1
#define O_RDWR     2
#define O_CREAT    0x40
#define O_TRUNC    0x200
#define O_APPEND   0x400
#define O_NONBLOCK 0x800

/* fcntl() commands */
#define F_GETFL 3
#define F_SETFL 4

static inline int syscall0(int num)
{
    int ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(num) : "memory");
    return ret;
}

static inline int syscall1(int num, unsigned int a)
{
    int ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(num), "b"(a)
                     : "memory");
    return ret;
}

static inline int syscall2(int num, unsigned int a, unsigned int b)
{
    int ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(num), "b"(a), "c"(b)
                     : "memory");
    return ret;
}

static inline int syscall3(int num, unsigned int a, unsigned int b, unsigned int c)
{
    int ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(num), "b"(a), "c"(b), "d"(c)
                     : "memory");
    return ret;
}

static inline int write(int fd, const char *buf, unsigned int len)
{
    return syscall3(SYS_WRITE, (unsigned int)fd, (unsigned int)buf, len);
}

static inline __attribute__((noreturn)) void exit(int code)
{
    syscall1(SYS_EXIT_TASK, (unsigned int)code);
    for (;;);
}

static inline int read(int fd, char *buf, unsigned int len)
{
    return syscall3(SYS_READ, (unsigned int)fd, (unsigned int)buf, len);
}

static inline int getpid(void)
{
    return syscall0(SYS_GETPID);
}

static inline int ioctl(int fd, unsigned int request, void *arg)
{
    return syscall3(SYS_IOCTL, (unsigned int)fd, request, (unsigned int)arg);
}

static inline int fcntl(int fd, int cmd, int arg)
{
    return syscall3(SYS_FCNTL, (unsigned int)fd, (unsigned int)cmd, (unsigned int)arg);
}


static inline int kill(unsigned int pid)
{
    return syscall1(SYS_KILL, pid);
}

static inline void yield(void)
{
    syscall0(SYS_YIELD);
}

/* fork: returns child PID in parent, 0 in child, -1 on error */
static inline int fork(void)
{
    return syscall0(SYS_FORK);
}

/* execv: replace current process image with path; argv is NULL-terminated.
 * Inherits the existing task environment (envp=NULL). */
static inline int execv(const char *path, char *const argv[])
{
    return syscall3(SYS_EXECVE, (unsigned int)path, (unsigned int)argv, 0);
}

/* execve: replace current process image; envp replaces the task environment */
static inline int execve(const char *path, char *const argv[], char *const envp[])
{
    return syscall3(SYS_EXECVE, (unsigned int)path,
                    (unsigned int)argv, (unsigned int)envp);
}

/* waitpid: wait for child pid to exit; writes exit code to *status if non-NULL.
 * Pass WNOHANG in flags to return -1 immediately if child hasn't exited. */
static inline int waitpid(int pid, int *status, int flags)
{
    return syscall3(SYS_WAITPID, (unsigned int)pid, (unsigned int)status,
                    (unsigned int)flags);
}

static inline int getppid(void)
{
    return syscall0(SYS_GETPPID);
}

/* VFS wrappers */
static inline int vfs_open(const char *path)
{
    return syscall2(SYS_OPEN, (unsigned int)path, O_RDONLY);
}

static inline int open(const char *path, int flags)
{
    return syscall2(SYS_OPEN, (unsigned int)path, (unsigned int)flags);
}

static inline int vfs_close(int fd)
{
    return syscall1(SYS_CLOSE, (unsigned int)fd);
}

static inline int vfs_read(int fd, void *buf, unsigned int len)
{
    return syscall3(SYS_READ, (unsigned int)fd, (unsigned int)buf, len);
}

static inline int vfs_lseek(int fd, unsigned int offset, int whence)
{
    return syscall3(SYS_LSEEK, (unsigned int)fd, offset, (unsigned int)whence);
}

static inline int vfs_readdir(int fd, struct vfs_dirent *de)
{
    return syscall2(SYS_READDIR, (unsigned int)fd, (unsigned int)de);
}

static inline int vfs_stat(const char *path, struct vfs_stat *st)
{
    return syscall2(SYS_STAT, (unsigned int)path, (unsigned int)st);
}

/* pipe: creates a pipe; fds[0]=read end, fds[1]=write end */
static inline int pipe(int fds[2])
{
    return syscall1(SYS_PIPE, (unsigned int)fds);
}

/* dup2: duplicate oldfd onto newfd; closes newfd first if open */
static inline int dup2(int oldfd, int newfd)
{
    return syscall2(SYS_DUP2, (unsigned int)oldfd, (unsigned int)newfd);
}

/* read_nb: non-blocking read — returns 0 immediately if pipe is empty */
static inline int read_nb(int fd, char *buf, unsigned int len)
{
    return syscall3(SYS_READ_NB, (unsigned int)fd, (unsigned int)buf, len);
}

/* task_done: non-blocking check — returns 1 if pid no longer exists */
static inline int task_done(int pid)
{
    return syscall1(SYS_TASK_DONE, (unsigned int)pid);
}

/* chdir: change current working directory; returns 0 or -1 */
static inline int chdir(const char *path)
{
    return syscall1(SYS_CHDIR, (unsigned int)path);
}

/* getcwd: copy current working directory into buf; returns 0 or -1 */
static inline int getcwd(char *buf, unsigned int len)
{
    return syscall2(SYS_GETCWD, (unsigned int)buf, len);
}

/* mkdir: create a new directory; returns 0 or -1 */
static inline int mkdir(const char *path)
{
    return syscall1(SYS_MKDIR, (unsigned int)path);
}

/* unlink: remove a regular file; returns 0 or -1 */
static inline int unlink(const char *path)
{
    return syscall1(SYS_UNLINK, (unsigned int)path);
}

/* mount: mount filesystem fstype at path; returns 0 or -1 */
static inline int mount(const char *fstype, const char *path)
{
    return syscall2(SYS_MOUNT, (unsigned int)fstype, (unsigned int)path);
}

/* mount_dev: mount block-device-backed filesystem; device is e.g. "/dev/hda1" */
static inline int mount_dev(const char *fstype, const char *device, const char *path)
{
    return syscall3(SYS_MOUNT, (unsigned int)fstype, (unsigned int)path,
                    (unsigned int)device);
}

/* umount: unmount filesystem at path; returns 0 or -1 */
static inline int umount(const char *path)
{
    return syscall1(SYS_UMOUNT, (unsigned int)path);
}

/* fstat: stat an open fd; returns 0 or -1 */
static inline int fstat(int fd, struct vfs_stat *st)
{
    return syscall2(SYS_FSTAT, (unsigned int)fd, (unsigned int)st);
}

/* rename: rename or move old_path to new_path; returns 0 or -1 */
static inline int rename(const char *old_path, const char *new_path)
{
    return syscall2(SYS_RENAME, (unsigned int)old_path, (unsigned int)new_path);
}

/* brk: set program break to addr; returns new break, or current break if addr==0 */
static inline unsigned int brk(unsigned int addr)
{
    return (unsigned int)syscall1(SYS_BRK, addr);
}

/* sbrk: increment program break by increment bytes; returns old break or (void*)-1 on failure */
static inline void *sbrk(int increment)
{
    unsigned int cur = (unsigned int)syscall1(SYS_BRK, 0);
    if (increment == 0)
        return (void *)cur;
    unsigned int newbrk = cur + (unsigned int)increment;
    unsigned int got = (unsigned int)syscall1(SYS_BRK, newbrk);
    return (got == newbrk) ? (void *)cur : (void *)-1;
}

