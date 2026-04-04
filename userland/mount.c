/* mount — display mounted filesystems */

#include "syscall.h"

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    mount();
    return 0;
}
