#ifndef KERNEL_H
#define KERNEL_H

#include <stdbool.h>

struct multiboot_info *mb_info;

void panic(char *msg);

/* printf() for the kernel */
int printk(const char *format, ...);

/* in cpu.c */
bool init_cpu(void);

#endif
