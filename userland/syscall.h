#pragma once

/* User-space syscall stubs for Luminary OS.
 * Convention: syscall number in EAX, args in EBX, ECX, EDX.
 * Return value in EAX. */

#define SYS_NOP         0
#define SYS_WRITE       1
#define SYS_EXIT        2   /* debug halt - do not use in userland */
#define SYS_READ        3
#define SYS_UPTIME      4
#define SYS_GETPID      5
#define SYS_HALT        6
#define SYS_PS          7
#define SYS_SPAWN       16  /* spawn(path) -> pid or -1 */
#define SYS_KILL        17  /* kill(pid) -> 0 or -1 */
#define SYS_YIELD       18  /* yield() - give up CPU slice */
/* VFS syscalls */
#define SYS_OPEN        21  /* open(path) -> fd or -1 */
#define SYS_CLOSE       22  /* close(fd) -> 0 or -1 */
#define SYS_READ_FD     23  /* read_fd(fd, buf, len) -> bytes or -1 */
#define SYS_LSEEK       24  /* lseek(fd, offset, whence) -> offset or -1 */
#define SYS_READDIR     25  /* readdir(fd, dirent_ptr) -> 1/0/-1 */
#define SYS_STAT        26  /* stat(path, stat_ptr) -> 0/-1 */
#define SYS_MOUNT       27  /* mount() -> prints mount table, returns 0 */
#define SYS_EXEC        28  /* exec(path, argv) - exec in-place -> 0 or -1 */
#define SYS_FORK        29  /* fork() -> child PID in parent, 0 in child */
#define SYS_WAITPID     30  /* waitpid(pid) -> pid on child exit, -1 on error */
#define SYS_EXIT_TASK   31  /* exit_task() - kill calling task */

/* VFS node type flags (must match kernel/vfs.h) */
#define VFS_FILE 0x01
#define VFS_DIR  0x02

/* struct vfs_dirent (must match kernel/vfs.h layout) */
#define VFS_NAME_MAX 128
struct vfs_dirent {
    char         name[VFS_NAME_MAX];
    unsigned int inode;
    unsigned char type;
};

/* struct vfs_stat (must match kernel/vfs.h layout) */
struct vfs_stat {
    unsigned int size;
    unsigned char type;
};

/* SEEK constants */
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

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

static inline int write(const char *buf, unsigned int len)
{
    return syscall2(SYS_WRITE, (unsigned int)buf, len);
}

static inline __attribute__((noreturn)) void exit(int code)
{
    (void)code;
    syscall0(SYS_EXIT_TASK);
    for (;;);
}

static inline int read(char *buf, unsigned int len)
{
    return syscall2(SYS_READ, (unsigned int)buf, len);
}

static inline int uptime(void)
{
    return syscall0(SYS_UPTIME);
}

static inline int getpid(void)
{
    return syscall0(SYS_GETPID);
}

static inline void halt(void)
{
    syscall0(SYS_HALT);
    for (;;);
}

static inline int ps(void)
{
    return syscall0(SYS_PS);
}

/* spawn: launch ELF by VFS path as a new task, returns new pid or -1 */
static inline int spawn(const char *path)
{
    return syscall1(SYS_SPAWN, (unsigned int)path);
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

/* execv: replace current process image with path; argv is NULL-terminated */
static inline int execv(const char *path, char *const argv[])
{
    return syscall2(SYS_EXEC, (unsigned int)path, (unsigned int)argv);
}

/* waitpid: block until child pid exits; returns pid on success, -1 on error */
static inline int waitpid(int pid)
{
    return syscall1(SYS_WAITPID, (unsigned int)pid);
}

/* VFS wrappers */
static inline int vfs_open(const char *path)
{
    return syscall1(SYS_OPEN, (unsigned int)path);
}

static inline int vfs_close(int fd)
{
    return syscall1(SYS_CLOSE, (unsigned int)fd);
}

static inline int vfs_read(int fd, void *buf, unsigned int len)
{
    return syscall3(SYS_READ_FD, (unsigned int)fd, (unsigned int)buf, len);
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

static inline int mount(void)
{
    return syscall0(SYS_MOUNT);
}
