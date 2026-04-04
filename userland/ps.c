/* ps — list running tasks */

#include "syscall.h"
#include "libc/stdio.h"

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    char buf[1024];
    int n = ps(buf, sizeof(buf));
    if (n > 0) write(1, buf, (unsigned int)n);
    return 0;
}
