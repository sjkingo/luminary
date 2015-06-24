/* Scheduling algorithm
 * ====================
 * The scheduler implements a basic hard priority-based preemptive algorithm.
 * Each task (T) is assigned two priority levels (initially equal): a static
 * priority (P) and a dynamic priority (p), both in the range of [1..10], with
 * 10 being the highest level. On each scheduling decision (t), the task with
 * the highest dynamic priority is chosen to run. If no clear decision can be
 * made (for example T(p=X) > 1, where X is a given priority) then a tie
 * breaker will be entered, as defined below.
 *
 * Tie breaks
 * ----------
 * If a tie break is entered, the scheduler will take a set of tasks in the
 * highest dynamic priority (the tied priority) and from that set pick the task
 * that ran most recently. This helps minimise thrashing by reducing the number
 * of context switches away from the previously running task. This behaviour is
 * noticed often in the example given below.
 *
 * Resource starvation
 * -------------------
 * To minimise the effects of resource starvation, each task's dynamic priority
 * is aged each time it runs. Initially each task has p=P. Each time a task
 * is run, its dynamic priority is decreased by one priority level until p=0,
 * at which point the task will be suspended until there are no scheduable
 * tasks left. It (and assuming other tasks) will then have their dynamic
 * priorities reset to their static priority and the aging process will begin
 * again.
 *
 * Priority inversion
 * ------------------
 * This suspend-and-reset process prevents a corner case of priority inversion,
 * because if tasks were reset to their static priority immediately as they hit
 * p=0, a lower priority task (which will always be aged out sooner) will be
 * given a higher dynamic priority than other tasks (which will have a higher
 * static priority): priority inversion.
 *
 * In the example below this can occur if at t=19, instead of suspending C, the
 * task is reset to its static priority of P=4. The state of the priority
 * tables would then be: A(p=1), B(p=1), C(p=4).
 *
 * System example
 * --------------
 * A contrived example of this algorithm is given.
 *      (where t=slice, p=dyn_priority, {A,B,C}=tasks, *..* means scheduled task)
 *
 * Three tasks are defined with static priorities:
 *      A(P=10)  B(P=7)  C(P=4)
 *
 *          A        B       C           Priorities at the scheduling decision
 *      /------------------------\
 * t=0  |   X    |       |       |       *A(p=10)*  B(p=7)   C(p=4)
 * t=1  |   X    |       |       |       *A(p=9)*   B(p=7)   C(p=4)
 * t=2  |   X    |       |       |       *A(p=8)*   B(p=7)   C(p=4)
 * t=3  |   X    |       |       |       *A(p=7)*   B(p=7)   C(p=4)     tie break
 * t=4  |        |   X   |       |        A(p=6)   *B(p=7)*  C(p=4)
 * t=5  |        |   X   |       |        A(p=6)   *B(p=6)*  C(p=4)     tie break
 * t=6  |   X    |       |       |       *A(p=6)*   B(p=5)   C(p=4)
 * t=7  |   X    |       |       |       *A(p=5)*   B(p=5)   C(p=4)     tie break
 * t=8  |        |   X   |       |        A(p=4)   *B(p=5)*  C(p=4)
 * t=9  |        |   X   |       |        A(p=4)   *B(p=4)*  C(p=4)     tie break
 * t=10 |   X    |       |       |       *A(p=4)*   B(p=3)   C(p=4)     tie break
 * t=11 |        |       |   X   |        A(p=3)    B(p=3)  *C(p=4)*
 * t=12 |        |       |   X   |        A(p=3)    B(p=3)  *C(p=3)*    tie break
 * t=13 |   X    |       |       |       *A(p=3)*   B(p=3)   C(p=2)     tie break
 * t=14 |        |   X   |       |        A(p=2)   *B(p=3)*  C(p=2)
 * t=15 |        |   X   |       |        A(p=2)   *B(p=2)*  C(p=2)     tie break
 * t=16 |   X    |       |       |       *A(p=2)*   B(p=1)   C(p=2)     tie break
 * t=17 |        |       |   X   |        A(p=1)    B(p=1)  *C(p=2)*
 * t=18 |        |       |   X   |        A(p=1)    B(p=1)  *C(p=1)*    tie break
 * t=19 |   X    |       | ~~~~~ |       *A(p=1)*   B(p=1)    N/A       C is suspended
 * t=20 | ~~~~~~ |   X   | ~~~~~ |         N/A     *B(p=1)*   N/A       A is suspended
 * t=21 | ~~~~~~ | ~~~~~ | ~~~~~ |         N/A       N/A      N/A       B is suspended
 *      
 * At t=21 no tasks are scheduable as they have all been suspended from aging
 * to 0, so all tasks are reset to their original priorities and the process
 * starts again. Assuming no new tasks are created t=21 will be the same as
 * t=0.
 *
 * An astute observer will notice that in a complete scheduling cycle
 * ([t=0..t=20], above) each task runs the same number of times as its static
 * priority. This creates a predictability of running times in the system.
 */

#include "kernel/kernel.h"
#include "sched.h"
#include "task.h"

/* XXX: future */
static struct task *queues[SCHED_QUEUE_HIGHEST][SCHED_QUEUE_MAX_TASKS_PER];

/* The currently running task, updated by the scheduler */
struct task *running_task;

/* The scheduling queue */
struct task *sched_queue;

/* Helper macro to return queue from queue level */
#define Q(x) (queues[(SCHED_QUEUE_HIGHEST-x)])

static void clear_queues(void)
{
    sched_queue = NULL;
    running_task = NULL;
    memset(queues, 0, sizeof(queues));
}

static void update_queue_statusline(void)
{
    char line[1024];
    line[0] = '\0';

    /* build up the queue line */
    struct task *t = sched_queue;
    do {
        char r = ' ';
        if (t == running_task) {
            r = '*';
        } else if (t->prio_d == SCHED_LEVEL_SUSP) {
            r = 'S';
        }
        sprintf(line, "%s%c%d:%s (p=%d)  ", line, r, t->pid, t->name, t->prio_d);
        t = t->next;
    } while (t != NULL);

    printsl(line);
}

/* Resets all task's dynamic priorities to their static priorities. */
static unsigned int reset_all_priorities(void)
{
    unsigned int r = 0;
    struct task *t = sched_queue;
    while (t != NULL) {
        if (t->pid == PID_IDLE)
            break;
        t->prio_d = t->prio_s;
        r++;
        t = t->next;
    }
    return r;
}

void sched(void)
{
    struct task *picked, *t;

    if (sched_queue == NULL)
        panic("BUG: no tasks to run as head of sched_queue is missing");

pick:
    picked = NULL;
    t = sched_queue;
    while (t != NULL) {
        /* skip over suspended tasks */
        if (t->prio_d == SCHED_LEVEL_SUSP) {
            goto next;
        }

        if ((t->next == NULL) || (t->prio_d > t->next->prio_d)) {
            /* if the next task has a lower priority than this one, stop looking */
            picked = t;
            break;
        } else if (t->prio_d == t->next->prio_d) {
            /* tie break as there are >1 tasks at this priority level */
            if (running_task == t) {
                picked = t;
                break;
            } else if (running_task == t->next) {
                picked = t->next;
                break;
            } else {
                panic("BUG: tie break failed");
            }
        }
next:
        t = t->next;
    }

    if (picked == NULL)
        panic("BUG: no tasks to schedule");

    /* if we picked the idle task, attempt to run other tasks instead */
    if (t->pid == PID_IDLE) {
        unsigned int r = reset_all_priorities();
        if (r > 0)
            goto pick;
        /* else run the idle task */
    } else {
        /* age this task */
        if (picked->prio_d > 0) {
            picked->prio_d--;
        } else if (picked->prio_d == SCHED_LEVEL_SUSP) {
            /* do nothing */
        }
    }

    /* TODO: run this task */
    running_task = picked;
    update_queue_statusline();
#ifdef DEBUG
    printk("%s ", picked->name);
#endif
}

void init_sched(void)
{
    clear_queues();
}
