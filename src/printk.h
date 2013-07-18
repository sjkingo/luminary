#ifndef PRINTK_H
#define PRINTK_H

/* printf() for the kernel */
int printk(const char *format, ...)
    __attribute__((format (printf, 1, 2)));

int sprintf(char *out, const char *format, ...)
    __attribute__((format (printf, 2, 3)));

int printsl(const char *format, ...)
    __attribute__((format (printf, 1, 2)));

#endif
