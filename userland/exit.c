/* exit — exit the current process */

#include "syscall.h"
#include "libc/stdlib.h"

int main(int argc, char **argv)
{
    int code = (argc > 1) ? (int)atou(argv[1]) : 0;
    exit(code);
    return 0;
}
