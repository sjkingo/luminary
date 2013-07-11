#ifndef TRAPS_H
#define TRAPS_H

#define NUM_TRAP_VECTORS 256

enum cpu_interrupts {
    INT_DIVIDE         = 0,    // divide by 0
    INT_DEBUG          = 1,    // debug exception
    INT_NMI            = 2,    // non-maskable interrupt
    INT_BREAK          = 3,    // breakpoint
    INT_OVERFL         = 4,    // overflow
    INT_BOUNDS_CHK     = 5,    // bounds check
    INT_ILLEGAL        = 6,    // illegal opcode
    INT_DEVICE         = 7,    // device not available
    INT_DOUBLEF        = 8,    // double fault
    // 9: reserved
    INT_TSS            = 10,   // tss invalid
    INT_SEGMENT        = 11,   // segment not present
    INT_STACK          = 12,   // stack exception
    INT_GPF            = 13,   // general protection fault
    INT_PAGE_FAULT     = 14,   // page fault
    // 15: reserved
    INT_FP_ERR         = 16,   // floating point error
    INT_ALIGN_CHK      = 17,   // alignment check
    INT_MACHINE_CHK    = 18,   // machine check
    INT_SIMD_ERR       = 19    // SIMD floating point error
};

struct trap_frame {
    /* register state */
    unsigned int ds;
    unsigned int edi;
    unsigned int esi;
    unsigned int ebp;
    unsigned int esp;
    unsigned int ebx;
    unsigned int edx;
    unsigned int ecx;
    unsigned int eax;

    unsigned int magic;
    unsigned int trapno;
    unsigned int err;

    unsigned int eip;
    unsigned int cs;
    unsigned int eflags;

    /* unused user-mode registers */
    unsigned int uesp;
    unsigned int uss;
};

#endif
