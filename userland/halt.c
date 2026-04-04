/* halt — shut down the system */

#include "syscall.h"

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    halt();
    return 0;
}
