/* mount — display or create filesystem mounts.
 *
 * Usage:
 *   mount                    — list all current mounts
 *   mount fstype path        — mount filesystem of type fstype at path
 *   mount fstype device path — mount block-device-backed filesystem
 */

#include "syscall.h"
#include "sys_dev.h"
#include "libc/stdio.h"

int main(int argc, char **argv)
{
    if (argc == 4) {
        if (mount_dev(argv[1], argv[2], argv[3]) != 0) {
            printf("mount: failed to mount %s (%s) at %s\n", argv[1], argv[2], argv[3]);
            return 1;
        }
        return 0;
    }

    if (argc == 3) {
        if (mount(argv[1], argv[2]) != 0) {
            printf("mount: failed to mount %s at %s\n", argv[1], argv[2]);
            return 1;
        }
        return 0;
    }

    if (argc != 1) {
        printf("usage: mount [fstype path] | [fstype device path]\n");
        return 1;
    }

    int sfd = open("/dev/sys", O_RDWR);
    if (sfd < 0) { printf("mount: cannot open /dev/sys\n"); return 1; }

    char buf[512];
    int n = sys_mounts(sfd, buf, sizeof(buf) - 1);
    vfs_close(sfd);

    if (n > 0)
        printf("%s", buf);

    return 0;
}
