#pragma once

#define SCHED_QUEUE_LOWEST 0
#define SCHED_QUEUE_HIGHEST 10
#define SCHED_QUEUE_MAX_TASKS_PER     16

/* The currently running task */
extern struct task *running_task;

/* The previously running task (set when a context switch is needed) */
extern struct task *prev_task;

/* Scheduling queue head */
extern struct task *sched_queue;

/* Scheduling levels. See the comment in sched.c for more info. */
#define SCHED_LEVEL_SUSP        0
#define SCHED_LEVEL_IDLE        (-1)
#define SCHED_LEVEL_MIN         1
#define SCHED_LEVEL_MAX         10

/* The scheduler */
void sched(void);

void init_sched(void);

/* Free the stale kernel stack of a just-killed task.
 * Called from traps.S after the ESP switch to the new task. */
void sched_free_stale_stack(void);
