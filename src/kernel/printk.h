#pragma once

/* printf() for the kernel — outputs to framebuffer console + serial */
int printk(const char *format, ...)
    __attribute__((format (printf, 1, 2)));

/* Serial-only printf — used for debug output, no framebuffer output */
int printk_serial(const char *format, ...)
    __attribute__((format (printf, 1, 2)));

int sprintf(char *out, const char *format, ...)
    __attribute__((format (printf, 2, 3)));

