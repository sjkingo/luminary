#include <stddef.h>

#include "kernel/kernel.h"
#include "kernel/elf.h"
#include "kernel/heap.h"
#include "kernel/pmm.h"
#include "kernel/sched.h"
#include "kernel/task.h"
#include "kernel/vmm.h"
#include "cpu/dt.h"
#include "cpu/traps.h"
#include "cpu/x86.h"

_Static_assert(offsetof(struct task, esp) == TASK_ESP_OFFSET,
               "TASK_ESP_OFFSET does not match struct layout");
_Static_assert(offsetof(struct task, page_dir_phys) == TASK_PAGE_DIR_OFFSET,
               "TASK_PAGE_DIR_OFFSET does not match struct layout");
_Static_assert(offsetof(struct task, stack_base) == TASK_STACK_BASE_OFFSET,
               "TASK_STACK_BASE_OFFSET does not match struct layout");

/* last PID allocated to a task */
static unsigned int last_pid = PID_IDLE;

/* The first task */
static struct task idle_task;

/* Init task and its ELF data for respawning */
static struct task task_init;
static const void *init_elf_data;
static unsigned int init_elf_size;

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
    t->page_dir_phys = vmm_create_page_dir();
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

#ifdef DEBUG
    printk("new_task: %s, pid=%d, created=%d, prio=%d\n", name, t->pid, t->created, prio);
#endif
}

/* Build a synthetic trap frame for a user-mode task. When trapret runs iret,
 * the CPU sees ring 3 CS and pops SS:ESP from the frame, switching to the
 * user stack. entry_point is the virtual address to begin execution at. */
static void setup_user_task_stack(struct task *t, uint32_t entry_point)
{
    unsigned char *stack = (unsigned char *)kmalloc(TASK_STACK_SIZE);
    if (stack == NULL)
        panic("setup_user_task_stack: kmalloc failed");

    t->stack_base = (unsigned int)stack;

    unsigned int *sp = (unsigned int *)(stack + TASK_STACK_SIZE);

    /* iret to ring 3 pops: EIP, CS, EFLAGS, ESP, SS (5 dwords) */
    *(--sp) = SEG_USER_DATA;             /* SS: user data segment */
    *(--sp) = USER_STACK_TOP;            /* ESP: top of user stack */
    *(--sp) = 0x202;                     /* EFLAGS: IF=1 */
    *(--sp) = SEG_USER_CODE;             /* CS: user code segment */
    *(--sp) = entry_point;               /* EIP: user entry point */

    /* skipped by addl $12 */
    *(--sp) = 0;                         /* err */
    *(--sp) = 0;                         /* trapno */
    *(--sp) = TRAP_MAGIC;                /* magic */

    /* popal: EAX, ECX, EDX, EBX, ESP(ignored), EBP, ESI, EDI */
    *(--sp) = 0;
    *(--sp) = 0;
    *(--sp) = 0;
    *(--sp) = 0;
    *(--sp) = 0;
    *(--sp) = 0;
    *(--sp) = 0;
    *(--sp) = 0;

    /* pop ds */
    *(--sp) = SEG_USER_DATA;             /* DS: user data segment */

    t->esp = (unsigned int)sp;
}

void create_user_task(struct task *t, char *name, int prio,
                      void *code, unsigned int code_size)
{
    if (prio < SCHED_LEVEL_MIN || prio > SCHED_LEVEL_MAX)
        panic("requested priority out of range");
    if (code_size > PAGE_SIZE)
        panic("create_user_task: code exceeds one page");

    disable_interrupts();

    t->name = name;
    t->pid = ++last_pid;
    t->created = timekeeper.uptime_ms;
    t->prio_s = prio;
    t->prio_d = prio;
    t->entry = NULL;
    t->page_dir_phys = vmm_create_page_dir();
    t->prev = NULL;
    t->next = NULL;

    /* Allocate a physical frame for user code and copy it there */
    uint32_t code_frame = pmm_alloc_frame();
    vmm_map_page(code_frame, code_frame, PTE_PRESENT | PTE_WRITE);
    memcpy((void *)code_frame, code, code_size);
    vmm_unmap_page(code_frame);
    /* Map it at USER_SPACE_START in the task's address space */
    vmm_map_page_in(t->page_dir_phys, USER_SPACE_START, code_frame,
                    PTE_PRESENT | PTE_USER);

    /* Allocate a physical frame for user stack */
    uint32_t stack_frame = pmm_alloc_frame();
    vmm_map_page(stack_frame, stack_frame, PTE_PRESENT | PTE_WRITE);
    memset((void *)stack_frame, 0, PAGE_SIZE);
    vmm_unmap_page(stack_frame);
    /* Map at USER_STACK_TOP - PAGE_SIZE .. USER_STACK_TOP */
    vmm_map_page_in(t->page_dir_phys, USER_STACK_TOP - PAGE_SIZE, stack_frame,
                    PTE_PRESENT | PTE_WRITE | PTE_USER);

    setup_user_task_stack(t, USER_SPACE_START);

    /* Insert into scheduler queue (same logic as create_task) */
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

    last->next = t;
    t->prev = last;

out:
    if (sched_queue == NULL)
        panic("BUG: head of sched_queue is empty");

#ifdef DEBUG
    printk("new_user_task: %s, pid=%d, created=%d, prio=%d\n",
           name, t->pid, t->created, prio);
#endif
}

/* Helper to insert a task into the scheduler queue by priority */
static void insert_task_sorted(struct task *t, int prio)
{
    struct task *before = sched_queue;
    struct task *last_node = before;
    do {
        if (prio >= before->prio_s) {
            insert_task_before(t, before);
            return;
        }
        last_node = before;
        before = before->next;
    } while (before != NULL);

    last_node->next = t;
    t->prev = last_node;
}

void create_elf_task(struct task *t, char *name, int prio,
                     const void *elf_data, unsigned int elf_size)
{
    if (prio < SCHED_LEVEL_MIN || prio > SCHED_LEVEL_MAX)
        panic("requested priority out of range");

    disable_interrupts();

    t->name = name;
    t->pid = ++last_pid;
    t->created = timekeeper.uptime_ms;
    t->prio_s = prio;
    t->prio_d = prio;
    t->entry = NULL;
    t->page_dir_phys = vmm_create_page_dir();
    t->prev = NULL;
    t->next = NULL;

    /* Load the ELF into the task's address space */
    uint32_t entry_point = elf_load(elf_data, elf_size, t->page_dir_phys);
    if (entry_point == 0)
        panic("create_elf_task: ELF load failed");

    /* Allocate a user stack */
    uint32_t stack_frame = pmm_alloc_frame();
    vmm_map_page(stack_frame, stack_frame, PTE_PRESENT | PTE_WRITE);
    memset((void *)stack_frame, 0, PAGE_SIZE);
    vmm_unmap_page(stack_frame);
    vmm_map_page_in(t->page_dir_phys, USER_STACK_TOP - PAGE_SIZE, stack_frame,
                    PTE_PRESENT | PTE_WRITE | PTE_USER);

    setup_user_task_stack(t, entry_point);
    insert_task_sorted(t, prio);

    if (sched_queue == NULL)
        panic("BUG: head of sched_queue is empty");

#ifdef DEBUG
    printk("new_elf_task: %s, pid=%d, entry=0x%lx, prio=%d\n",
           name, t->pid, entry_point, prio);
#endif
}

void spawn_init(const void *elf_data, unsigned int elf_size)
{
    init_elf_data = elf_data;
    init_elf_size = elf_size;
    create_elf_task(&task_init, "init", 5, elf_data, elf_size);
    task_init.pid = PID_INIT;
}

static void respawn_init(void)
{
    printk("init: respawning\n");
    memset(&task_init, 0, sizeof(task_init));
    create_elf_task(&task_init, "init", 5, init_elf_data, init_elf_size);
    task_init.pid = PID_INIT;
}

void task_kill(struct task *t)
{
    if (t->pid == PID_IDLE)
        panic("task_kill: cannot kill idle task");

    printk("sched: removing '%s' (pid %d) from run queue\n", t->name, t->pid);

    /* unlink from scheduler queue */
    if (t->prev != NULL)
        t->prev->next = t->next;
    else
        sched_queue = t->next;

    if (t->next != NULL)
        t->next->prev = t->prev;

    /* free kernel stack */
    if (t->stack_base != 0)
        kfree((void *)t->stack_base);

    /* destroy task's page directory (frees user page tables and frames) */
    if (t->page_dir_phys != vmm_get_kernel_page_dir())
        vmm_destroy_page_dir(t->page_dir_phys);

    /* if the killed task is init, respawn it immediately */
    if (t == &task_init)
        respawn_init();

    /* if we killed the running task, force a context switch.
     * We pick a new task and jump directly to its saved state,
     * bypassing the normal scheduler path since the current
     * trap frame belongs to the dead task. */
    if (t == running_task) {
        /* pick the first schedulable task */
        struct task *next = sched_queue;
        while (next != NULL && next->prio_d == SCHED_LEVEL_SUSP)
            next = next->next;
        if (next == NULL) {
            /* all suspended - reset priorities and pick again */
            next = sched_queue;
            while (next != NULL) {
                if (next->pid != PID_IDLE)
                    next->prio_d = next->prio_s;
                next = next->next;
            }
            next = sched_queue;
        }
        if (next == NULL)
            panic("task_kill: no tasks left");

        prev_task = NULL;
        running_task = next;

        /* switch to the new task's address space and stack, then
         * jump to trapret to restore its saved context */
        vmm_switch_page_dir(running_task->page_dir_phys);
        tss_set_kernel_stack(running_task->stack_base + TASK_STACK_SIZE);

        asm volatile(
            "movl %0, %%esp\n\t"
            "jmp trapret\n\t"
            :
            : "r"(running_task->esp)
        );
        __builtin_unreachable();
    }
}

void init_task(void)
{
    /* hand-craft the first task (uses the boot stack) */
    memset(&idle_task, 0, sizeof(idle_task));
    idle_task.name = "idle";
    idle_task.pid = PID_IDLE;
    idle_task.prio_s = idle_task.prio_d = SCHED_LEVEL_IDLE;
    idle_task.esp = 0;              /* filled when first switched out */
    idle_task.page_dir_phys = vmm_get_kernel_page_dir();
    idle_task.stack_base = 0;       /* boot stack, not from heap */
    idle_task.entry = NULL;     /* already running */
    sched_queue = &idle_task;
    running_task = &idle_task;
}
