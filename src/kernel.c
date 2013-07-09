#include "kernel.h"
#include "vga.h"

void kernel_main(void)
{
    init_vga();
    while(1);
}
