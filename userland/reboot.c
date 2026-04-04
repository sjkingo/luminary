/* reboot — restart the system */

#include "syscall.h"

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    reboot();
    return 0;
}
