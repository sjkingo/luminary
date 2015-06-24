#pragma once

/* A task to be scheduled in the system */
struct task {
    char *name;
    unsigned int pid;
    unsigned int created; /* ms since boot */

    int prio_s; /* static priority */
    int prio_d; /* dynamic priority */

    struct task *prev, *next;
};

#define PID_IDLE                1

/* Initialise the task subsystem */
void init_task(void);

/* Create a new task and insert it into the queue */
void create_task(struct task *t, char *name, int prio);
