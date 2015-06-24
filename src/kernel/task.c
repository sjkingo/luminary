#include "kernel/kernel.h"
#include "kernel/sched.h"
#include "task.h"

/* last PID allocated to a task */
static unsigned int last_pid = PID_IDLE;

/* The first task */
static struct task idle_task;

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

void create_task(struct task *t, char *name, int prio)
{
    if (prio < SCHED_LEVEL_MIN || prio > SCHED_LEVEL_MAX)
        panic("requested priority out of range");

    t->name = name;
    t->pid = ++last_pid;
    t->created = timekeeper.uptime_ms;

    t->prio_s = prio;
    t->prio_d = prio;

    t->prev = NULL;
    t->next = NULL;

    /* figure out where to add this task */
    struct task *before = sched_queue;
    do {
        if (prio >= before->prio_s) {
            insert_task_before(t, before);
            goto out;
        }
        before = before->next;
    } while (before != NULL);
    panic("BUG: could not find a place in the queue for this task");

out:
    if (sched_queue == NULL)
        panic("BUG: head of sched_queue is empty");
#ifdef DEBUG
    printk("new_task: %s, pid=%d, created=%d, prio=%d\n", name, t->pid, t->created, prio);
#endif
}

void init_task(void)
{
    /* hand-craft the first task */
    memset(&idle_task, 0, sizeof(idle_task));
    idle_task.name = "idle";
    idle_task.pid = PID_IDLE;
    idle_task.prio_s = idle_task.prio_d = SCHED_LEVEL_IDLE;
    sched_queue = &idle_task;
}
