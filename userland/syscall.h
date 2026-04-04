#pragma once

/* User-space syscall stubs for Luminary OS.
 * Convention: syscall number in EAX, args in EBX, ECX, EDX.
 * Return value in EAX. */

#define SYS_NOP         0
#define SYS_EXIT_TASK   1   /* exit_task() - kill calling task */
#define SYS_READ        3
#define SYS_WRITE       4
#define SYS_OPEN        5   /* open(path) -> fd or -1 */
#define SYS_CLOSE       6   /* close(fd) -> 0 or -1 */
#define SYS_PS          7   /* ps(buf, len) -> bytes written or -1 */
#define SYS_WIN_CREATE      8
#define SYS_WIN_DESTROY     9
#define SYS_WIN_FILL_RECT   10
#define SYS_WIN_DRAW_TEXT   11
#define SYS_WIN_FLIP        12
#define SYS_WIN_POLL_EVENT  13
#define SYS_MOUSE_GET       14
#define SYS_WIN_DRAW_RECT   15
#define SYS_WIN_GET_SIZE    19
#define SYS_GETPID      20  /* getpid() -> pid */
#define SYS_HALT        21  /* halt() - shut down */
#define SYS_UPTIME      22  /* uptime() -> ms */
#define SYS_SPAWN       16  /* spawn(path) -> pid or -1 */
#define SYS_KILL        17  /* kill(pid) -> 0 or -1 */
#define SYS_YIELD       18  /* yield() - give up CPU slice */
#define SYS_LSEEK       24  /* lseek(fd, offset, whence) -> offset or -1 */
#define SYS_READDIR     25  /* readdir(fd, dirent_ptr) -> 1/0/-1 */
#define SYS_STAT        26  /* stat(path, stat_ptr) -> 0/-1 */
#define SYS_MOUNT       27  /* mount() -> prints mount table, returns 0 */
#define SYS_EXEC        28  /* exec(path, argv) - exec in-place -> 0 or -1 */
#define SYS_FORK        29  /* fork() -> child PID in parent, 0 in child */
#define SYS_WAITPID     30  /* waitpid(pid) -> pid on child exit, -1 on error */
#define SYS_PIPE        32  /* pipe(int fds[2]) -> 0 or -1 */
#define SYS_DUP2        33  /* dup2(oldfd, newfd) -> newfd or -1 */

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
    unsigned int size;
    unsigned char type;
};

/* SEEK constants */
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

/* open() flags — Linux i386 values */
#define O_RDONLY  0
#define O_WRONLY  1
#define O_RDWR    2
#define O_CREAT   0x40
#define O_TRUNC   0x200
#define O_APPEND  0x400

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
    (void)code;
    syscall0(SYS_EXIT_TASK);
    for (;;);
}

static inline int read(int fd, char *buf, unsigned int len)
{
    return syscall3(SYS_READ, (unsigned int)fd, (unsigned int)buf, len);
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

static inline int ps(char *buf, unsigned int len)
{
    return syscall2(SYS_PS, (unsigned int)buf, len);
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

static inline int mount(void)
{
    return syscall0(SYS_MOUNT);
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
