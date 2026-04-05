/* mkdir — create a directory */

#include "syscall.h"
#include "libc/stdio.h"

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: mkdir dir...\n");
        return 1;
    }

    int ret = 0;
    for (int i = 1; i < argc; i++) {
        if (mkdir(argv[i]) != 0) {
            printf("mkdir: cannot create directory '%s'\n", argv[i]);
            ret = 1;
        }
    }
    return ret;
}
