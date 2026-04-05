/* uptime — print system uptime */

#include "syscall.h"
#include "sys_dev.h"
#include "libc/stdio.h"

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    int sfd = open("/dev/sys", O_RDWR);
    if (sfd < 0) { printf("uptime: cannot open /dev/sys\n"); exit(1); }

    unsigned int ms  = sys_uptime(sfd);
    unsigned int s   = ms / 1000;
    unsigned int m   = s / 60;
    unsigned int h   = m / 60;
    s %= 60; m %= 60;
    printf("up %u:%02u:%02u\n", h, m, s);

    vfs_close(sfd);
    return 0;
}
