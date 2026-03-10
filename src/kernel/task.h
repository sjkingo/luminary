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
    unsigned int page_dir_phys; /* physical address of task's page directory */
    unsigned int stack_base;    /* base of allocated stack (for kfree) */
    void (*entry)(void);        /* entry point for new tasks */

    struct task *prev, *next;
};

#define PID_IDLE                0
#define PID_INIT                1
#define TASK_STACK_SIZE         4096
#define TASK_ESP_OFFSET         24  /* byte offset of esp in struct task */
#define TASK_PAGE_DIR_OFFSET    28  /* byte offset of page_dir_phys in struct task */
#define TASK_STACK_BASE_OFFSET  32  /* byte offset of stack_base in struct task */

/* Initialise the task subsystem */
void init_task(void);

/* Create a new kernel task and insert it into the queue */
void create_task(struct task *t, char *name, int prio, void (*entry)(void));

/* Create a user-mode task. The code at entry (size code_size bytes) is copied
 * to a new physical frame and mapped at USER_SPACE_START in the task's address
 * space. A user stack is allocated at USER_STACK_TOP. The task starts in ring 3. */
void create_user_task(struct task *t, char *name, int prio,
                      void *code, unsigned int code_size);

/* Create a user-mode task from an ELF binary loaded in memory.
 * The ELF's PT_LOAD segments are mapped into the task's address space
 * and execution begins at the ELF entry point. */
void create_elf_task(struct task *t, char *name, int prio,
                     const void *elf_data, unsigned int elf_size);

/* Spawn init from ELF data in the initrd. Saves the ELF location so init
 * can be respawned automatically if it dies (like Unix PID 1). */
void spawn_init(const void *elf_data, unsigned int elf_size);

/* Kill a task: remove from scheduler queue, free resources, reschedule.
 * Must be called with interrupts disabled. If the killed task is the
 * currently running task, this function does not return.
 * If the killed task is init, it is respawned automatically. */
void task_kill(struct task *t);
