#include <stddef.h>
#include <string.h>

#include "kernel/kernel.h"
#include "kernel/dev.h"
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

    strncpy(t->name, name, sizeof(t->name) - 1);
    t->name[sizeof(t->name) - 1] = '\0';
    t->pid = ++last_pid;
    t->created = timekeeper.uptime_ms;

    t->prio_s = prio;
    t->prio_d = prio;

    t->entry = entry;
    t->page_dir_phys = vmm_create_page_dir();
    t->prev = NULL;
    t->next = NULL;
    memset(t->fds, 0, sizeof(t->fds));
    task_open_std_fds(t);
    t->ppid      = 0;
    t->wait_pid  = -1;
    t->wait_done = false;

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

    DBGK("task", "new_task: %s pid=%d prio=%d\n", name, t->pid, prio);
}

/* Build a synthetic trap frame for a user-mode task. When trapret runs iret,
 * the CPU sees ring 3 CS and pops SS:ESP from the frame, switching to the
 * user stack. entry_point is the virtual address to begin execution at.
 * user_sp is the initial user-mode stack pointer (set up by elf_load). */
static void setup_user_task_stack(struct task *t, uint32_t entry_point, uint32_t user_sp)
{
    unsigned char *stack = (unsigned char *)kmalloc(TASK_STACK_SIZE);
    if (stack == NULL)
        panic("setup_user_task_stack: kmalloc failed");

    t->stack_base = (unsigned int)stack;

    unsigned int *sp = (unsigned int *)(stack + TASK_STACK_SIZE);

    /* iret to ring 3 pops: EIP, CS, EFLAGS, ESP, SS (5 dwords) */
    *(--sp) = SEG_USER_DATA;             /* SS: user data segment */
    *(--sp) = user_sp;                   /* ESP: initial user stack pointer */
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

    strncpy(t->name, name, sizeof(t->name) - 1);
    t->name[sizeof(t->name) - 1] = '\0';
    t->pid = ++last_pid;
    t->created = timekeeper.uptime_ms;
    t->prio_s = prio;
    t->prio_d = prio;
    t->entry = NULL;
    t->page_dir_phys = vmm_create_page_dir();
    t->prev = NULL;
    t->next = NULL;
    memset(t->fds, 0, sizeof(t->fds));
    task_open_std_fds(t);
    t->ppid      = 0;
    t->wait_pid  = -1;
    t->wait_done = false;

    /* Allocate a physical frame for user code and copy it there */
    uint32_t code_frame = pmm_alloc_frame();
    void *kp_code = vmm_kmap(code_frame);
    memcpy(kp_code, code, code_size);
    vmm_kunmap(kp_code);
    /* Map it at USER_SPACE_START in the task's address space */
    vmm_map_page_in(t->page_dir_phys, USER_SPACE_START, code_frame,
                    PTE_PRESENT | PTE_USER);

    /* Allocate a physical frame for user stack */
    uint32_t stack_frame = pmm_alloc_frame();
    void *kp_stack = vmm_kmap(stack_frame);
    memset(kp_stack, 0, PAGE_SIZE);
    vmm_kunmap(kp_stack);
    /* Map at USER_STACK_TOP - PAGE_SIZE .. USER_STACK_TOP */
    vmm_map_page_in(t->page_dir_phys, USER_STACK_TOP - PAGE_SIZE, stack_frame,
                    PTE_PRESENT | PTE_WRITE | PTE_USER);

    setup_user_task_stack(t, USER_SPACE_START, USER_STACK_TOP);

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

    DBGK("task", "new_user_task: %s pid=%d prio=%d\n", name, t->pid, prio);
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

    strncpy(t->name, name, sizeof(t->name) - 1);
    t->name[sizeof(t->name) - 1] = '\0';
    t->pid = ++last_pid;
    t->created = timekeeper.uptime_ms;
    t->prio_s = prio;
    t->prio_d = prio;
    t->entry = NULL;
    t->page_dir_phys = vmm_create_page_dir();
    t->prev = NULL;
    t->next = NULL;
    memset(t->fds, 0, sizeof(t->fds));
    task_open_std_fds(t);
    t->ppid      = 0;
    t->wait_pid  = -1;
    t->wait_done = false;

    /* Load the ELF into the task's address space (also allocates stack) */
    uint32_t user_sp = 0;
    uint32_t entry_point = elf_load(elf_data, elf_size, t->page_dir_phys,
                                    0, NULL, &user_sp);
    if (entry_point == 0)
        panic("create_elf_task: ELF load failed");

    setup_user_task_stack(t, entry_point, user_sp);
    insert_task_sorted(t, prio);

    if (sched_queue == NULL)
        panic("BUG: head of sched_queue is empty");

    DBGK("task", "new_elf_task: %s pid=%d entry=0x%lx prio=%d\n",
         name, t->pid, entry_point, prio);
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
    /* Note: always called from within task_kill(), which has already called
     * disable_interrupts(). The window between memset and create_elf_task
     * (where a zeroed task_init could be visible to the scheduler) is
     * therefore safe — no timer IRQ can fire here. */
    printk("init: respawning\n");
    memset(&task_init, 0, sizeof(task_init));
    create_elf_task(&task_init, "init", 5, init_elf_data, init_elf_size);
    task_init.pid = PID_INIT;
}

/* Stale kernel stack to free after switching away from a dying task.
 * Set by task_kill when killing the running task; freed by
 * sched_free_stale_stack() which is called from traps.S after the ESP
 * switch, so we are safely on the new task's stack when kfree runs. */
static void *stale_stack = NULL;

/* Called from traps.S (after movl running_task->esp, %%esp) to free the
 * old kernel stack of a just-killed task.  Must be a proper C function so
 * the DEBUG kfree macro expands correctly. */
void sched_free_stale_stack(void)
{
    if (stale_stack) {
        void *p = stale_stack;
        stale_stack = NULL;
        kfree(p);
    }
}

void (*task_death_hook)(uint32_t pid) = NULL;

void task_kill(struct task *t)
{
    if (t->pid == PID_IDLE)
        panic("task_kill: cannot kill idle task");

    /* Disable interrupts while manipulating the scheduler queue.
     * Callers that already have interrupts disabled (e.g. sys_kill, the
     * compositor self-kill path) calling disable_interrupts() again is
     * harmless on this single-core x86 kernel. */
    disable_interrupts();

    DBGK("sched", "removing '%s' (pid %d) from run queue\n", t->name, t->pid);

    /* unlink from scheduler queue */
    if (t->prev != NULL)
        t->prev->next = t->next;
    else
        sched_queue = t->next;

    if (t->next != NULL)
        t->next->prev = t->prev;

    /* notify subsystems (e.g. GUI) that this pid is dying */
    if (task_death_hook)
        task_death_hook(t->pid);

    /* destroy task's page directory (frees user page tables and frames) */
    if (t->page_dir_phys != vmm_get_kernel_page_dir())
        vmm_destroy_page_dir(t->page_dir_phys);

    /* if the killed task is init, respawn it immediately */
    if (t == &task_init)
        respawn_init();

    /* Wake any task waiting for this pid */
    {
        unsigned int dead_pid = t->pid;
        struct task *waiter = sched_queue;
        while (waiter) {
            if (waiter->wait_pid == (int)dead_pid) {
                waiter->wait_pid  = -1;
                waiter->wait_done = true;
                waiter->prio_d    = waiter->prio_s; /* unsuspend */
            }
            waiter = waiter->next;
        }
    }

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
         * jump to trapret to restore its saved context.
         * We must NOT free the dying task's kernel stack before switching
         * ESP — a spurious/NMI interrupt between kfree and the movl would
         * use the freed stack. Free it after we are on the new stack. */
        uint32_t old_stack = t->stack_base;
        t->stack_base = 0;

        vmm_switch_page_dir(running_task->page_dir_phys);
        tss_set_kernel_stack(running_task->stack_base + TASK_STACK_SIZE);

        /* Queue the old stack for freeing — traps.S will call
         * sched_free_stale_stack() after switching to the new ESP. */
        stale_stack = (void *)old_stack;

        asm volatile(
            "movl %0, %%esp\n\t"
            "jmp sched_enter_trapret\n\t"
            :
            : "r"(running_task->esp)
        );
        __builtin_unreachable();
    }

    /* Killed a different task — free its stack and re-enable interrupts. */
    if (t->stack_base != 0)
        kfree((void *)t->stack_base);
    enable_interrupts();
}

struct task *task_fork(struct trap_frame *frame)
{
    struct task *child = (struct task *)kmalloc(sizeof(struct task));
    if (!child) {
        printk("fork: out of memory\n");
        return NULL;
    }

    disable_interrupts();

    /* Copy most fields from parent */
    *child = *running_task;

    child->pid    = ++last_pid;
    child->ppid   = running_task->pid;
    child->wait_pid  = -1;
    child->wait_done = false;
    child->prev   = NULL;
    child->next   = NULL;

    /* Allocate the child's kernel stack BEFORE cloning the page directory,
     * so the new slab mapping is in the kernel page directory at clone time
     * and the child's copy inherits it. */
    uint8_t *kstack = (uint8_t *)kmalloc(TASK_STACK_SIZE);
    if (!kstack) {
        kfree(child);
        enable_interrupts();
        printk("fork: out of memory for kernel stack\n");
        return NULL;
    }

    /* Deep-copy the address space (kernel page dir now includes kstack mapping) */
    child->page_dir_phys = vmm_clone_page_dir(running_task->page_dir_phys);
    uint8_t *parent_kstack = (uint8_t *)running_task->stack_base;
    memcpy(kstack, parent_kstack, TASK_STACK_SIZE);

    child->stack_base = (unsigned int)kstack;

    /* Use the current syscall trap frame as the saved ESP.
     * running_task->esp is only updated on context switch, so it may be
     * stale. The frame pointer IS the current kernel stack position. */
    uint32_t parent_frame_esp = (uint32_t)frame;
    uint32_t stack_offset = parent_frame_esp - running_task->stack_base;
    child->esp = child->stack_base + stack_offset;

    DBGK("task", "fork: parent frame_esp=0x%lx stack_base=0x%x offset=0x%lx\n",
         parent_frame_esp, running_task->stack_base, stack_offset);
    DBGK("task", "fork: child  esp=0x%x stack_base=0x%x\n",
         child->esp, child->stack_base);

    /* Set child's return value (EAX in trap frame) to 0.
     * The trap frame is embedded in the kernel stack, so we
     * find it at the same offset as in the parent's stack. */
    struct trap_frame *child_frame = (struct trap_frame *)child->esp;
    DBGK("task", "fork: child_frame->ds=0x%x magic=0x%x eax=0x%x eip=0x%x cs=0x%x uesp=0x%x\n",
         child_frame->ds, child_frame->magic, child_frame->eax,
         child_frame->eip, child_frame->cs, child_frame->uesp);
    child_frame->eax = 0;

    /* Debug: peek at top of child's and parent's user stack */
    {
        uint32_t uesp = child_frame->uesp;
        uint32_t pdi = uesp >> 22;
        uint32_t pti = (uesp >> 12) & 0x3FF;
        uint32_t off = uesp & 0xFFF;
        uint32_t *cdir = (uint32_t *)child->page_dir_phys;
        uint32_t *pdir = (uint32_t *)running_task->page_dir_phys;
        if (cdir[pdi] & 1) {
            uint32_t *cpt = (uint32_t *)(cdir[pdi] & 0xFFFFF000);
            uint32_t cframe = cpt[pti] & 0xFFFFF000;
            void *kp = vmm_kmap(cframe);
            uint32_t cval = *(uint32_t *)((uint8_t *)kp + off);
            vmm_kunmap(kp);
            DBGK("task", "fork: child  stack[0x%lx] = 0x%lx frame=0x%lx\n", uesp, cval, cframe);
        }
        if (pdir[pdi] & 1) {
            uint32_t *ppt = (uint32_t *)(pdir[pdi] & 0xFFFFF000);
            uint32_t pframe = ppt[pti] & 0xFFFFF000;
            void *kp = vmm_kmap(pframe);
            uint32_t pval = *(uint32_t *)((uint8_t *)kp + off);
            vmm_kunmap(kp);
            DBGK("task", "fork: parent stack[0x%lx] = 0x%lx frame=0x%lx\n", uesp, pval, pframe);
        }
    }

    /* Verify child trap frame EIP just before scheduling */
    {
        struct trap_frame *cf = (struct trap_frame *)child->esp;
        DBGK("task", "fork: pre-sched child_frame eip=0x%lx ds=0x%lx esp=0x%lx\n",
             (uint32_t)cf->eip, (uint32_t)cf->ds, (uint32_t)cf->esp);
    }

    /* Insert child into scheduler after parent */
    insert_task_sorted(child, child->prio_s);

    enable_interrupts();

    DBGK("task", "fork: pid %d -> child pid %d\n", running_task->pid, child->pid);
    return child;
}

int task_exec(const void *elf_data, uint32_t elf_size, struct trap_frame *frame,
              int argc, const char *const *argv)
{
    /* Capture task name from argv[0] before the address space is replaced.
     * argv[0] is a user-space pointer valid in the current page directory. */
    if (argc > 0 && argv[0]) {
        /* Use basename: find last '/' */
        const char *base = argv[0];
        for (const char *p = argv[0]; *p; p++)
            if (*p == '/')
                base = p + 1;
        strncpy(running_task->name, base, sizeof(running_task->name) - 1);
        running_task->name[sizeof(running_task->name) - 1] = '\0';
    }

    /* Create a new page directory for the new image */
    uint32_t new_dir = vmm_create_page_dir();

    uint32_t new_sp = 0;
    uint32_t entry = elf_load(elf_data, elf_size, new_dir, argc, argv, &new_sp);
    if (entry == 0) {
        vmm_destroy_page_dir(new_dir);
        return -1;
    }

    /* Free the old address space */
    uint32_t old_dir = running_task->page_dir_phys;

    /* Switch to new address space before destroying the old one */
    running_task->page_dir_phys = new_dir;
    vmm_switch_page_dir(new_dir);

    vmm_destroy_page_dir(old_dir);

    /* Reset the trap frame for the new image.
     * The frame is on the kernel stack - rewrite it in place. */
    frame->eip    = entry;
    frame->cs     = SEG_USER_CODE;
    frame->eflags = 0x202;           /* IF=1 */
    frame->uesp   = new_sp;
    frame->uss    = SEG_USER_DATA;
    frame->eax    = 0;
    frame->ebx    = 0;
    frame->ecx    = 0;
    frame->edx    = 0;
    frame->esi    = 0;
    frame->edi    = 0;
    frame->ebp    = 0;
    frame->ds     = SEG_USER_DATA;

    /* Update TSS kernel stack pointer */
    tss_set_kernel_stack(running_task->stack_base + TASK_STACK_SIZE);

    return 0;
}

void init_task(void)
{
    /* hand-craft the first task (uses the boot stack) */
    memset(&idle_task, 0, sizeof(idle_task));
    strncpy(idle_task.name, "idle", sizeof(idle_task.name) - 1);
    idle_task.name[sizeof(idle_task.name) - 1] = '\0';
    idle_task.pid = PID_IDLE;
    idle_task.prio_s = idle_task.prio_d = SCHED_LEVEL_IDLE;
    idle_task.esp = 0;              /* filled when first switched out */
    idle_task.page_dir_phys = vmm_get_kernel_page_dir();
    idle_task.stack_base = 0;       /* boot stack, not from heap */
    idle_task.entry = NULL;         /* already running */
    idle_task.ppid      = 0;
    idle_task.wait_pid  = -1;
    idle_task.wait_done = false;
    sched_queue = &idle_task;
    running_task = &idle_task;
}
