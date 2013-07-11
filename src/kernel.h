#ifndef KERNEL_H
#define KERNEL_H

struct multiboot_info *mb_info;

/* Kernel panic: print given message and halt the CPU */
void panic(char *msg)
    __attribute__((noreturn));

/* printf() for the kernel */
int printk(const char *format, ...)
    __attribute__((format (printf, 1, 2)));

/* in cpu.c */
void init_cpu(void);

#endif
