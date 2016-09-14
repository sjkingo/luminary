#pragma once

#include <stdbool.h>

/* x86-specific routines for accessing registers and I/O ports.
 */

static inline void outb(unsigned short port, unsigned char data)
{
    asm volatile("out %0, %1" : : "a" (data), "d" (port));
}

static inline void outb_16(unsigned short port, unsigned short data)
{
    asm volatile("outw %1, %0" : : "dN" (port), "a" (data));
}

static inline void outb_32(unsigned int port, unsigned long data)
{
    asm volatile("outl %0, %w1" : : "a" (data), "d" (port));
}

static inline unsigned char inb(unsigned short port)
{
    unsigned char data;
    asm volatile("in %1, %0" : "=a" (data) : "d" (port));
    return data;
}

static inline unsigned short inb_16(unsigned short port)
{
    unsigned short data;
    asm volatile("inw %1, %0" : "=a" (data) : "d" (port));
    return data;
}

static inline unsigned int inb_32(unsigned short port) {
    unsigned int data;
    asm volatile ("inl %%dx, %%eax" : "=a" (data) : "dN" (port));
    return data;
}

static inline int get_cr0(void)
{
    int cr0;
    asm volatile("mov %%cr0, %%eax\n"
                 "mov %%eax, %0": "=m" (cr0));
    return cr0;
}

static inline bool in_protected_mode(void)
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

#define disable_interrupts() ({ asm volatile("cli\nnop"); })
#define enable_interrupts() ({ asm volatile("sti\nnop"); })

#define INT_1M (1024*1024*1024)
