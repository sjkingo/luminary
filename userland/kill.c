/* kill — terminate a task by PID */

#include "syscall.h"
#include "libc/stdio.h"
#include "libc/stdlib.h"

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: kill <pid>\n");
        return 1;
    }
    unsigned int pid = atou(argv[1]);
    if (kill(pid) < 0) {
        printf("kill: failed (pid not found?)\n");
        return 1;
    }
    return 0;
}
