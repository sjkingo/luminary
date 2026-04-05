/* mv — move or rename files and directories */

#include "syscall.h"
#include "libc/stdio.h"

int main(int argc, char **argv)
{
    if (argc != 3) {
        printf("usage: mv <src> <dst>\n");
        return 1;
    }
    if (rename(argv[1], argv[2]) < 0) {
        printf("mv: cannot move '%s' to '%s'\n", argv[1], argv[2]);
        return 1;
    }
    return 0;
}
