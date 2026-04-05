/* reboot — restart the system */

#include "syscall.h"
#include "sys_dev.h"
#include "libc/stdio.h"

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    int sfd = open("/dev/sys", O_RDWR);
    if (sfd < 0) { printf("reboot: cannot open /dev/sys\n"); exit(1); }
    sys_reboot(sfd);
    return 0;
}
