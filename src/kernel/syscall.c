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
#include "cpu/x86.h"
#include "drivers/keyboard.h"

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

static int sys_read(struct trap_frame *frame)
{
    char *buf = (char *)frame->ebx;
    unsigned int len = frame->ecx;

    if (buf == NULL || len > 4096)
        return -1;

    int n = keyboard_read(buf, len);
    if (n == 0) {
        /* Buffer empty - briefly enable interrupts so pending IRQs
         * (keyboard, timer) can fire before we return to userspace. */
        enable_interrupts();
        asm volatile("hlt");
        disable_interrupts();
    }
    return n;
}

static int sys_uptime(void)
{
    return (int)timekeeper.uptime_ms;
}

static int sys_getpid(void)
{
    return (int)running_task->pid;
}

static int sys_halt(void)
{
    printk("system halted by pid %d\n", running_task->pid);
    extern void cpu_halt(void);
    cpu_halt();
    return 0; /* unreachable */
}

static int sys_ps(void)
{
    struct task *t = sched_queue;
    printk("PID  PRIO     NAME\n");
    while (t != NULL) {
        printk("%-4d %-8d %s\n", t->pid, t->prio_s, t->name);
        t = t->next;
    }
    return 0;
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
    case SYS_READ:
        ret = sys_read(frame);
        break;
    case SYS_UPTIME:
        ret = sys_uptime();
        break;
    case SYS_GETPID:
        ret = sys_getpid();
        break;
    case SYS_HALT:
        ret = sys_halt();
        break;
    case SYS_PS:
        ret = sys_ps();
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
