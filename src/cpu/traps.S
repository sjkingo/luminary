# vectors.s calls into here

#include "traps.h"

#define TASK_ESP_OFFSET 52
#define TASK_PAGE_DIR_OFFSET 56
#define TASK_STACK_BASE_OFFSET 60
#define TASK_STACK_HWM_OFFSET 64
#define TASK_STACK_SIZE 16384

.section .bss
.align 16
sched_saved_esp:        .space 4
                        .space 4096     # scratch stack for sched_free_stale_stack call
sched_scratch_stack_top:

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

    # update stack high-water mark (lowest ESP seen = most stack used)
    movl running_task, %eax
    testl %eax, %eax
    jz .Lhwm_skip
    cmpl %esp, TASK_STACK_HWM_OFFSET(%eax)   # if current esp < hwm, update
    jbe .Lhwm_skip
    movl %esp, TASK_STACK_HWM_OFFSET(%eax)
.Lhwm_skip:

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

    # update TSS kernel stack for ring transitions (before loading new ESP)
    movl TASK_STACK_BASE_OFFSET(%eax), %ecx
    addl $TASK_STACK_SIZE, %ecx
    pushl %ecx
    call tss_set_kernel_stack
    addl $4, %esp

    # load new task's ESP (after all stack operations)
    movl running_task, %eax
    movl TASK_ESP_OFFSET(%eax), %esp

    # clear prev_task
    movl $0, prev_task

    # fall through into sched_enter_trapret

# Entry point for task_kill: ESP already points at the new task's trap frame.
# Free the stale kernel stack on a private scratch stack so we don't corrupt
# the trap frame, then restore ESP and fall into trapret.
.globl sched_enter_trapret
sched_enter_trapret:
    # Save the new task's frame pointer, switch to scratch stack, call free,
    # then restore frame pointer before restoring context.
    movl %esp, sched_saved_esp
    movl $sched_scratch_stack_top, %esp
    call sched_free_stale_stack
    movl sched_saved_esp, %esp

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

    iret
