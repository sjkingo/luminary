#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "kernel/vfs.h"
#include "cpu/traps.h"

/* ── per-task open file descriptor table ─────────────────────────────────── */
/* Stored inline in struct task to avoid heap allocation on task creation.   */

/* Written to task.magic on creation, checked at every queue operation.
 * Poisoned to TASK_MAGIC_DEAD when the task struct is freed so use-after-free
 * is caught immediately rather than silently corrupting the scheduler queue. */
#define TASK_MAGIC      0xCA11AB1Eu
#define TASK_MAGIC_DEAD 0xDEADDEADu

/* A task to be scheduled in the system */
struct task {
    uint32_t magic;             /* TASK_MAGIC — checked at every queue operation */
    char name[32];
    unsigned int pid;
    unsigned int created; /* ms since boot */

    int prio_s; /* static priority */
    int prio_d; /* dynamic priority */

    unsigned long switched_in_ms; /* uptime_ms when this task was last switched in */

    unsigned int esp;           /* saved stack pointer */
    unsigned int page_dir_phys; /* physical address of task's page directory */
    unsigned int stack_base;    /* base of allocated stack (for kfree) */
    unsigned int stack_hwm;     /* lowest ESP seen (high-water mark of usage) */
    void (*entry)(void);        /* entry point for new tasks */

    /* Open file descriptor table */
    struct vfs_fd fds[VFS_FD_MAX];

    unsigned int ppid;          /* parent PID (0 if no parent) */
    int          wait_pid;      /* pid this task is blocked waiting for (-1 = not waiting) */
    bool         wait_done;     /* set true when the waited-on child has exited */
    int          exit_status;   /* exit code passed to SYS_EXIT_TASK; read by parent via SYS_WAITPID */

    uint32_t     ticks;         /* total scheduler ticks consumed by this task */
    uint32_t     ticks_window;  /* ticks accumulated in the current 1s window */
    uint32_t     cpu_pct;       /* CPU% from the last completed 1s window */

    unsigned int fault_count;   /* consecutive CPU exceptions; panic at MAX_TASK_FAULTS */
    bool         read_nonblock; /* set during SYS_READ_NB: chardev/pipe reads return 0 if empty */
    bool         blocking;      /* true while task is in a blocking hlt loop; ticks not charged */

    char cwd[256];              /* current working directory (absolute path) */
    char cmdline[128];          /* full argv[0..n] joined by spaces, for ps */

    uint32_t brk;               /* current program break (top of heap); 0 for kernel tasks */

    struct task *prev, *next;
};

#define SYS_BRK                 50

#define PID_IDLE                0
#define PID_INIT                1
#define TASK_STACK_SIZE         16384
#define TASK_ESP_OFFSET         56  /* byte offset of esp in struct task */
#define TASK_PAGE_DIR_OFFSET    60  /* byte offset of page_dir_phys in struct task */
#define TASK_STACK_BASE_OFFSET  64  /* byte offset of stack_base in struct task */
#define TASK_STACK_HWM_OFFSET   68  /* byte offset of stack_hwm in struct task */

/* Walk the scheduler queue and panic on any structural inconsistency.
 * Called at every queue mutation site. */
void sched_queue_verify(void);

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

/* Optional hook called by task_kill with the pid of the dying task.
 * Subsystems (e.g. GUI) register here to clean up per-task resources.
 * Called before the context switch, with interrupts disabled. */
extern void (*task_death_hook)(uint32_t pid);

/* Fork the current task: clone address space, return new task ptr.
 * Child gets EAX=0 in its trap frame. Parent gets child PID returned. */
struct task *task_fork(struct trap_frame *frame);

/* Exec-in-place: replace the running task's address space with a new ELF.
 * Frees old page directory, loads new ELF, resets trap frame.
 * argc/argv are set up on the new user stack.
 * Returns 0 on success, -1 on failure (task continues with old image on failure). */
int task_exec(const void *elf_data, uint32_t elf_size, struct trap_frame *frame,
              int argc, const char *const *argv);
