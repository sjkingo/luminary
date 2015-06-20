
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
