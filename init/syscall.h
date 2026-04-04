#pragma once

/* User-space syscall stubs for Luminary OS.
 * Convention: syscall number in EAX, args in EBX, ECX, EDX.
 * Return value in EAX. */

#define SYS_NOP         0
#define SYS_EXIT_TASK   1
#define SYS_READ        3
#define SYS_WRITE       4
#define SYS_OPEN        5
#define SYS_CLOSE       6
#define SYS_PS          7
#define SYS_SPAWN       16
#define SYS_KILL        17
#define SYS_YIELD       18
#define SYS_GETPID      20
#define SYS_HALT        21
#define SYS_UPTIME      22
#define SYS_LSEEK       24
#define SYS_READDIR     25
#define SYS_STAT        26
#define SYS_MOUNT       27

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

static inline int write(int fd, const char *buf, unsigned int len)
{
    return syscall3(SYS_WRITE, (unsigned int)fd, (unsigned int)buf, len);
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

/* ps: format process list into buf, returns bytes written or -1 */
static inline int ps(char *buf, unsigned int len)
{
    return syscall2(SYS_PS, (unsigned int)buf, len);
}

/* exec: spawn ELF by VFS path, returns new pid or -1 */
static inline int exec(const char *path)
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
