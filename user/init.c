/* First user-mode program for Luminary OS.
 * Prints a message via syscall and spins. */

#include "syscall.h"

static unsigned int strlen(const char *s)
{
    unsigned int len = 0;
    while (s[len])
        len++;
    return len;
}

static void puts(const char *s)
{
    write(s, strlen(s));
}

void _start(void)
{
    char **n = 0x0;
    puts("hello from user mode!\n");
    puts(*n);

    /* spin */
    for (;;)
        ;
}
