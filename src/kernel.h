#ifndef KERNEL_H
#define KERNEL_H

#include <stddef.h>
#include <string.h>

#include "printk.h"

struct multiboot_info *mb_info;

struct kernel_time {
    unsigned long uptime_ms;
};
extern struct kernel_time timekeeper;

#define panic(msg) (real_panic(msg, __FILE__, __LINE__, __func__))

/* Kernel panic: print given message and halt the CPU */
void real_panic(char *msg, char const *file, int line, char const *func)
    __attribute__((noreturn));

/* in cpu.c */
void init_cpu(void);

#endif
