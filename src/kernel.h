#ifndef KERNEL_H
#define KERNEL_H

struct multiboot_info *mb_info;

void panic(char *msg);

/* printf() for the kernel */
int printk(const char *format, ...);

/* in cpu.c */
void init_cpu(void);

#endif
