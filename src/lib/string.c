#include <sys/types.h>

void memset(void *dest, int c, int len)
{
    unsigned char b = (unsigned char)c;
    unsigned int w = ((unsigned int)b)       |
                     ((unsigned int)b << 8)  |
                     ((unsigned int)b << 16) |
                     ((unsigned int)b << 24);
    unsigned int *d32 = (unsigned int *)dest;
    int words = len >> 2;
    while (words--)
        *d32++ = w;
    char *p = (char *)d32;
    int tail = len & 3;
    while (tail--)
        *p++ = (char)b;
}

void *memcpy(void *dest, const void *src, int n)
{
    /* Use 32-bit stores for the bulk — ~4x faster than byte loop for large
     * MMIO blits (back→fb_hw).  Tail handles any non-dword remainder. */
    unsigned int *d32 = (unsigned int *)dest;
    const unsigned int *s32 = (const unsigned int *)src;
    int words = n >> 2;
    while (words--)
        *d32++ = *s32++;
    char *d = (char *)d32;
    const char *s = (const char *)s32;
    int tail = n & 3;
    while (tail--)
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

char *strncpy(char *dest, const char *src, int n)
{
    char *d = dest;
    while (n-- > 0) {
        *d = *src ? *src++ : '\0';
        d++;
    }
    return dest;
}
