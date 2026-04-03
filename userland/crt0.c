/* Minimal C runtime startup for Luminary userspace.
 * The kernel pushes [argc, argv[0..n-1], NULL] onto the user stack
 * before jumping to _start. This matches the i386 SysV ABI initial stack. */

#include "syscall.h"

int main(int argc, char **argv);

void __attribute__((noreturn)) _start(int argc, char **argv)
{
    int ret = main(argc, argv);
    exit(ret);
    for (;;);
}
