#pragma once

#include <stdarg.h>

int vsnprintf(char *buf, unsigned int size, const char *fmt, va_list args);
int snprintf(char *buf, unsigned int size, const char *fmt, ...);
int sprintf(char *buf, const char *fmt, ...);
int printf(const char *fmt, ...);

int puts(const char *s);
int putchar(int c);
