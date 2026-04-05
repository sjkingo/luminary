/* mount — display mounted filesystems */

#include "syscall.h"
#include "sys_dev.h"
#include "libc/stdio.h"

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    int sfd = open("/dev/sys", O_RDWR);
    if (sfd < 0) { printf("mount: cannot open /dev/sys\n"); exit(1); }

    char buf[512];
    int n = sys_mounts(sfd, buf, sizeof(buf) - 1);
    vfs_close(sfd);

    if (n > 0)
        printf("%s", buf);

    return 0;
}
