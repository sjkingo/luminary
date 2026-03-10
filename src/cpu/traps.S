# vectors.s calls into here

#include "traps.h"

#define TASK_ESP_OFFSET 24
#define TASK_PAGE_DIR_OFFSET 28
#define TASK_STACK_BASE_OFFSET 32
#define TASK_STACK_SIZE 4096

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

    # call the C handler with pointer to trap frame
    pushl %esp
    call trap_handler
    addl $4, %esp

    # check if scheduler requested a context switch
    movl prev_task, %eax
    testl %eax, %eax
    jz trapret

    # save current ESP to prev_task->esp
    movl %esp, TASK_ESP_OFFSET(%eax)

    # switch to new task's page directory
    movl running_task, %eax
    movl TASK_PAGE_DIR_OFFSET(%eax), %ecx
    movl %ecx, %cr3

    # load new task's ESP
    movl TASK_ESP_OFFSET(%eax), %esp

    # update TSS kernel stack for ring transitions
    movl TASK_STACK_BASE_OFFSET(%eax), %ecx
    addl $TASK_STACK_SIZE, %ecx
    pushl %ecx
    call tss_set_kernel_stack
    addl $4, %esp

    # clear prev_task
    movl $0, prev_task

.globl trapret
trapret:
    # restore segment registers and general purpose registers
    pop %ebx
    mov %bx, %ds
    mov %bx, %es
    mov %bx, %fs
    mov %bx, %gs
    popal

    # throw away magic, trapno and err from the stack and return
    addl $12, %esp
    sti

    iret
