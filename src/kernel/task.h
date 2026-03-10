#pragma once

/* A task to be scheduled in the system */
struct task {
    char *name;
    unsigned int pid;
    unsigned int created; /* ms since boot */

    int prio_s; /* static priority */
    int prio_d; /* dynamic priority */

    unsigned long switched_in_ms; /* uptime_ms when this task was last switched in */

    unsigned int esp;           /* saved stack pointer */
    unsigned int stack_base;    /* base of allocated stack (for kfree) */
    void (*entry)(void);        /* entry point for new tasks */

    struct task *prev, *next;
};

#define PID_IDLE                1
#define TASK_STACK_SIZE         4096
#define TASK_ESP_OFFSET         24  /* byte offset of esp in struct task */

/* Initialise the task subsystem */
void init_task(void);

/* Create a new task and insert it into the queue */
void create_task(struct task *t, char *name, int prio, void (*entry)(void));
