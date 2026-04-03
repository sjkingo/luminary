#pragma once

/* User-space syscall stubs for Luminary OS.
 * Convention: syscall number in EAX, args in EBX, ECX, EDX.
 * Return value in EAX. */

#define SYS_NOP     0
#define SYS_WRITE   1
#define SYS_EXIT    2
#define SYS_READ    3
#define SYS_UPTIME  4
#define SYS_GETPID  5
#define SYS_HALT    6
#define SYS_PS      7
#define SYS_EXEC    16  /* exec(module_index) -> pid or -1 */
#define SYS_YIELD   18  /* yield() - give up CPU slice */

static inline int syscall0(int num)
{
    int ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(num) : "memory");
    return ret;
}

static inline int syscall2(int num, unsigned int arg1, unsigned int arg2)
{
    int ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(num), "b"(arg1), "c"(arg2)
                     : "memory");
    return ret;
}

static inline int write(const char *buf, unsigned int len)
{
    return syscall2(SYS_WRITE, (unsigned int)buf, len);
}

static inline void exit(void)
{
    syscall0(SYS_EXIT);
    for (;;);  /* unreachable */
}

static inline int read(char *buf, unsigned int len)
{
    return syscall2(SYS_READ, (unsigned int)buf, len);
}

static inline int uptime(void)
{
    return syscall0(SYS_UPTIME);
}

static inline void yield(void)
{
    syscall0(SYS_YIELD);
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

static inline int exec(unsigned int mod_index)
{
    int ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(SYS_EXEC), "b"(mod_index)
                     : "memory");
    return ret;
}
