#ifndef X86_H
#define X86_H

#include <stdbool.h>

/* x86-specific routines for accessing registers and I/O ports.
 */

static inline void outb(unsigned short port, unsigned char data)
{
    asm volatile("out %0, %1" : : "a" (data), "d" (port));
}

static inline unsigned char inb(unsigned short port)
{
    unsigned char data;
    asm volatile("in %1, %0" : "=a" (data) : "d" (port));
    return data;
}

static inline int get_cr0(void)
{
    int cr0;
    asm volatile("mov %%cr0, %%eax\n"
                 "mov %%eax, %0": "=m" (cr0));
    return cr0;
}

static inline char in_protected_mode(void)
{
    /* bit 0 = PE
     * https://en.wikipedia.org/wiki/Control_register#CR0
     */
    int cr0 = get_cr0();
    return cr0 & (1<<0);
}

static inline bool is_paging_enabled(void)
{
    /* bit 16 = PG
     * https://en.wikipedia.org/wiki/Control_register#CR0
     */
    int cr0 = get_cr0();
    return cr0 & (1<<16);
}

#endif
