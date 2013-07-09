#ifndef KERNEL_H
#define KERNEL_H

/* Start and end addresses of the kernel, as provided by the linker */
extern int _kernel_start;
extern int _kernel_end;

/* printf() for the kernel */
int printk(const char *format, ...);

#endif
