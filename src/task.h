#ifndef TASK_H
#define TASK_H

/* A task to be scheduled in the system */
struct task {
    char *name;
    unsigned int pid;
    unsigned int created; /* ms since boot */

    int prio_s; /* static priority */
    int prio_d; /* dynamic priority */

    struct task *prev, *next;
};
extern struct task *sched_queue;
extern struct task *running_task;

/* Scheduling levels. See the comment in sched.c for more info. */
#define SCHED_LEVEL_SUSP        (-1)
#define SCHED_LEVEL_IDLE        0
#define SCHED_LEVEL_MIN         1
#define SCHED_LEVEL_MAX         10

#define PID_IDLE                1

/* The scheduler (in sched.c) */
void sched(void);

/* Initialise the task subsystem */
void init_task(void);

/* Create a new task and insert it into the queue */
void create_task(struct task *t, char *name, int prio);

#endif
