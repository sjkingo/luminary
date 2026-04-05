/* umount — unmount a filesystem */

#include "syscall.h"
#include "libc/stdio.h"

int main(int argc, char **argv)
{
    if (argc != 2) {
        printf("usage: umount path\n");
        return 1;
    }

    if (umount(argv[1]) != 0) {
        printf("umount: failed to unmount %s\n", argv[1]);
        return 1;
    }

    return 0;
}
