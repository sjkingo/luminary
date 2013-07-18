
void memset(void *dest, int c, int len)
{
    char *p = dest;
    while (len--)
        *p++ = c;
}
