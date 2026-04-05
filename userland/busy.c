/* busy - run a busy loop for testing cpu tracking */

#include "libc/stdio.h"

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    printf("doing busy work\n");
    while (1);
    return 0;
}
