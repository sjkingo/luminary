/* uptime — print system uptime */

#include "syscall.h"
#include "sys_dev.h"
#include "libc/stdio.h"

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    int sfd = open("/dev/sys", O_RDWR);
    if (sfd < 0) { printf("uptime: cannot open /dev/sys\n"); exit(1); }

    unsigned int ms   = sys_uptime(sfd);
    unsigned int secs = ms / 1000;
    unsigned int frac = (ms % 1000) / 100;
    printf("Up %u.%us\n", secs, frac);

    vfs_close(sfd);
    return 0;
}
