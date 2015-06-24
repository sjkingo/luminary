#ifndef TRAPS_H
#define TRAPS_H

#define TRAP_MAGIC 0xc0ffee

#ifndef __ASSEMBLER__

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
};

static inline char *resolve_vector_name(int v)
{
    switch (v) {
        case INT_DIVIDE:
            return "divide by 0";
        case INT_DEBUG:
            return "debug exception";
        case INT_NMI:
            return "non-maskable interrupt";
        case INT_BREAK:
            return "breakpoint";
        case INT_OVERFL:
            return "overflow";
        case INT_BOUNDS_CHK:
            return "bounds check";
        case INT_ILLEGAL:
            return "illegal opcode";
        case INT_DEVICE:
            return "device not available";
        case INT_DOUBLEF:
            return "double fault";
        case INT_TSS:
            return "invalid TSS";
        case INT_SEGMENT:
            return "segment not present";
        case INT_STACK:
            return "stack exception";
        case INT_GPF:
            return "general protection fault";
        case INT_PAGE_FAULT:
            return "page fault";
        case INT_FP_ERR:
            return "floating point error";
        default:
            return "??";
    }
}
#define VECTOR_NAME(v) resolve_vector_name(v)

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

    unsigned int magic; // == TRAP_MAGIC to be a valid frame
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

#endif
