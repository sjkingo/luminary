/* Syscall dispatch - handles int 0x80 from user mode.
 *
 * Convention: syscall number in EAX, arguments in EBX, ECX, EDX.
 * Return value placed in EAX of the trap frame.
 */

#include "kernel/kernel.h"
#include "kernel/syscall.h"
#include "kernel/task.h"
#include "kernel/sched.h"
#include "cpu/traps.h"

static int sys_nop(void)
{
    return 0;
}

static int sys_write(struct trap_frame *frame)
{
    /* EBX = buffer pointer, ECX = length */
    const char *buf = (const char *)frame->ebx;
    unsigned int len = frame->ecx;

    /* Basic validation: reject null pointers and unreasonable lengths */
    if (buf == NULL || len > 4096)
        return -1;

    /* Print character by character (safe, no format string issues) */
    for (unsigned int i = 0; i < len; i++)
        printk("%c", buf[i]);

    return (int)len;
}

static int sys_exit(void)
{
    printk("task '%s' (pid %d) called exit()\n",
           running_task->name, running_task->pid);
    while (1)
        asm volatile("hlt");
    return 0; /* unreachable */
}

void syscall_handler(struct trap_frame *frame)
{
    int ret;

    switch (frame->eax) {
    case SYS_NOP:
        ret = sys_nop();
        break;
    case SYS_WRITE:
        ret = sys_write(frame);
        break;
    case SYS_EXIT:
        ret = sys_exit();
        break;
    default:
        printk("unknown syscall %d from pid %d\n",
               frame->eax, running_task->pid);
        ret = -1;
        break;
    }

    /* Return value goes in EAX */
    frame->eax = (unsigned int)ret;
}
