#pragma once

/* User-space syscall stubs for Luminary OS.
 * Convention: syscall number in EAX, args in EBX, ECX, EDX.
 * Return value in EAX. */

#define SYS_NOP     0
#define SYS_WRITE   1
#define SYS_EXIT    2

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
