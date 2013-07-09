#ifndef KERNEL_H
#define KERNEL_H

struct multiboot_info *mb_info;

/* printf() for the kernel */
int printk(const char *format, ...);

#endif
