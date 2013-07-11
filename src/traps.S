# vectors.s calls into here

#include "traps.h"

.section .text

.globl alltraps
alltraps:
    cli
    # magic constant gets embedded in the trap_frame
    pushl $TRAP_MAGIC

    # trap frame
    pushal
    mov %ds, %ax
    pushl %eax
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs

    # call the C handler
    call trap_handler

    # clean up
    pop %ebx
    mov %bx, %ds
    mov %bx, %es
    mov %bx, %fs
    mov %bx, %gs
    popal

    # throw away eax, ds and magic from the stack and return
    addl $12, %esp
    sti

    iret
