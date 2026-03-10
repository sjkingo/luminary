#include "kernel/kernel.h"
#include "kernel/heap.h"
#include "kernel/sched.h"
#include "kernel/task.h"
#include "cpu/traps.h"
#include "cpu/x86.h"

/* last PID allocated to a task */
static unsigned int last_pid = PID_IDLE;

/* The first task */
static struct task idle_task;

/* Wrapper that calls the task's entry point and halts if it returns */
static void task_wrapper(void)
{
    running_task->entry();

    /* task returned - halt */
    printk("task '%s' (pid %d) exited\n", running_task->name, running_task->pid);
    while (1)
        asm volatile("hlt");
}

/* Build a synthetic interrupt frame on a new stack so trapret can start the task */
static void setup_task_stack(struct task *t)
{
    unsigned char *stack = (unsigned char *)kmalloc(TASK_STACK_SIZE);
    if (stack == NULL)
        panic("setup_task_stack: kmalloc failed");

    t->stack_base = (unsigned int)stack;

    /* Start at the top of the allocated stack */
    unsigned int *sp = (unsigned int *)(stack + TASK_STACK_SIZE);

    /* Build the frame that trapret will pop (high to low address).
     * trapret does: pop ds, popal, addl $12, sti, iret
     * iret pops: EIP, CS, EFLAGS
     * addl $12 skips: magic, trapno, err
     * popal pops: EDI, ESI, EBP, (ESP ignored), EBX, EDX, ECX, EAX
     * pop ds pops: DS
     */

    /* iret frame */
    *(--sp) = 0x202;                    /* EFLAGS: IF=1 */
    *(--sp) = 0x08;                     /* CS: kernel code segment */
    *(--sp) = (unsigned int)task_wrapper; /* EIP */

    /* skipped by addl $12 */
    *(--sp) = 0;                        /* err */
    *(--sp) = 0;                        /* trapno */
    *(--sp) = TRAP_MAGIC;               /* magic */

    /* popal: EAX, ECX, EDX, EBX, ESP(ignored), EBP, ESI, EDI */
    *(--sp) = 0;                        /* EAX */
    *(--sp) = 0;                        /* ECX */
    *(--sp) = 0;                        /* EDX */
    *(--sp) = 0;                        /* EBX */
    *(--sp) = 0;                        /* ESP (ignored by popal) */
    *(--sp) = 0;                        /* EBP */
    *(--sp) = 0;                        /* ESI */
    *(--sp) = 0;                        /* EDI */

    /* pop ds */
    *(--sp) = 0x10;                     /* DS: kernel data segment */

    t->esp = (unsigned int)sp;
}

static void insert_task_before(struct task *new_task, struct task *new_next)
{
    struct task *new_prev = new_next->prev;

    /* stitch to the task after this new one */
    new_task->next = new_next;
    new_next->prev = new_task;

    /* stitch to the task before this new one */
    if (new_prev == NULL) {
        /* head of queue */
        new_task->prev = NULL;
        sched_queue = new_task;
    } else {
        new_prev->next = new_task;
        new_task->prev = new_prev;
    }
}

void create_task(struct task *t, char *name, int prio, void (*entry)(void))
{
    if (prio < SCHED_LEVEL_MIN || prio > SCHED_LEVEL_MAX)
        panic("requested priority out of range");

    /* Disable interrupts while modifying the scheduler queue, since the
     * timer interrupt calls sched() which reads sched_queue. */
    disable_interrupts();

    t->name = name;
    t->pid = ++last_pid;
    t->created = timekeeper.uptime_ms;

    t->prio_s = prio;
    t->prio_d = prio;

    t->entry = entry;
    t->prev = NULL;
    t->next = NULL;

    setup_task_stack(t);

    /* figure out where to add this task */
    struct task *before = sched_queue;
    struct task *last = before;
    do {
        if (prio >= before->prio_s) {
            insert_task_before(t, before);
            goto out;
        }
        last = before;
        before = before->next;
    } while (before != NULL);

    /* No insertion point found - append after the last node */
    last->next = t;
    t->prev = last;

out:
    if (sched_queue == NULL)
        panic("BUG: head of sched_queue is empty");

    enable_interrupts();

#ifdef DEBUG
    printk("new_task: %s, pid=%d, created=%d, prio=%d\n", name, t->pid, t->created, prio);
#endif
}

void init_task(void)
{
    /* hand-craft the first task (uses the boot stack) */
    memset(&idle_task, 0, sizeof(idle_task));
    idle_task.name = "idle";
    idle_task.pid = PID_IDLE;
    idle_task.prio_s = idle_task.prio_d = SCHED_LEVEL_IDLE;
    idle_task.esp = 0;          /* filled when first switched out */
    idle_task.stack_base = 0;   /* boot stack, not from heap */
    idle_task.entry = NULL;     /* already running */
    sched_queue = &idle_task;
    running_task = &idle_task;
}
