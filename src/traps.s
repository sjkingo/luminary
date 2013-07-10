# vectors.s calls into here

.section .text

.globl alltraps
alltraps:
    # trap frame
    pushl %ds
    pushl %es
    pushl %fs
    pushl %gs
    pushal

    # load kernel segment descriptor
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs

    # call the C trap handler
    pushl %esp
    call trap_handler
    addl $4, %esp

    # fall through to trapret
.globl trapret
trapret:
    popal
    popl %gs
    popl %fs
    popl %es
    popl %ds
    addl $0x8, %esp
    iret
