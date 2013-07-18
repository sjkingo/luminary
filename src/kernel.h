#ifndef KERNEL_H
#define KERNEL_H

#include "printk.h"

struct multiboot_info *mb_info;

/* Kernel panic: print given message and halt the CPU */
void panic(char *msg)
    __attribute__((noreturn));

/* in cpu.c */
void init_cpu(void);

#endif
