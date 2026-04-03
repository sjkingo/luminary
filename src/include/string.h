#pragma once

#include <stddef.h>

void memset(void *dest, int c, int len);

void *memcpy(void *dest, const void *src, int n);

int memcmp(const void *s1, const void *s2, int n);

size_t strlen(const char *s);

int strcmp(const char *s1, const char *s2);

int strncmp(const char *s1, const char *s2, int n);

char *strchr(const char *s, int c);

char *strncpy(char *dest, const char *src, int n);
