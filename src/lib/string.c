#include <sys/types.h>

void memset(void *dest, int c, int len)
{
    char *p = dest;
    while (len--)
        *p++ = c;
}

void *memcpy(void *dest, const void *src, int n)
{
    char *d = (char *)dest;
    char *s = (char *)src;
    while (n--)
        *d++ = *s++;
    return dest;
}

size_t strlen(const char *s)
{
    size_t len = 0;
    while (*s++) len++;
    return len;
}

int memcmp(const void *s1, const void *s2, int n)
{
    const unsigned char *a = s1, *b = s2;
    while (n--) {
        if (*a != *b) return (int)*a - (int)*b;
        a++; b++;
    }
    return 0;
}

int strcmp(const char *s1, const char *s2)
{
    while (*s1 && *s1 == *s2) { s1++; s2++; }
    return (unsigned char)*s1 - (unsigned char)*s2;
}

int strncmp(const char *s1, const char *s2, int n)
{
    while (n-- > 0) {
        if (*s1 != *s2) return (unsigned char)*s1 - (unsigned char)*s2;
        if (*s1 == '\0') return 0;
        s1++; s2++;
    }
    return 0;
}

char *strchr(const char *s, int c)
{
    while (*s) {
        if (*s == (char)c) return (char *)s;
        s++;
    }
    return (c == '\0') ? (char *)s : 0;
}
