/* uptime — print system uptime */

#include "syscall.h"
#include "libc/stdio.h"

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    unsigned int ms   = (unsigned int)uptime();
    unsigned int secs = ms / 1000;
    unsigned int frac = (ms % 1000) / 100;
    printf("Up %u.%us\n", secs, frac);
    return 0;
}
