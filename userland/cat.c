/* cat — print file contents to stdout */

#include "syscall.h"
#include "libc/stdio.h"

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: cat <file> [file...]\n");
        return 1;
    }

    for (int i = 1; i < argc; i++) {
        int fd = vfs_open(argv[i]);
        if (fd < 0) {
            printf("cat: cannot open '%s'\n", argv[i]);
            continue;
        }
        char buf[512];
        int n;
        while ((n = vfs_read(fd, buf, sizeof(buf))) > 0)
            write(1, buf, (unsigned int)n);
        vfs_close(fd);
    }
    return 0;
}
