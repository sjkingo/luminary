#include <stddef.h>
#include <string.h>

#include "kernel/kernel.h"
#include "kernel/dev.h"
#include "kernel/elf.h"
#include "kernel/pipe.h"
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
_Static_assert(offsetof(struct task, stack_hwm) == TASK_STACK_HWM_OFFSET,
               "TASK_STACK_HWM_OFFSET does not match struct layout");

/* Verify a task struct has a valid magic value.
 * Use the TASK_CHECK macro so the panic shows the call site, not this helper. */
#define TASK_CHECK(t) do { \
    if ((t)->magic == TASK_MAGIC_DEAD) \
        panic("use-after-free: task struct accessed after kill"); \
    if ((t)->magic != TASK_MAGIC) \
        panic("task struct magic corrupted"); \
} while (0)

/* Walk the entire scheduler queue and verify structural integrity:
 * - every node has a valid magic value
 * - prev/next cross-links are consistent
 * - idle_task is reachable (it must always be in the queue)
 * Called at every queue mutation site and at sched() entry. */
void sched_queue_verify(void)
{
    struct task *t = sched_queue;
    struct task *prev = NULL;
    int count = 0;
    bool found_idle = false;

    while (t != NULL) {
        if (++count > 1024)
            panic("sched_queue_verify: queue loop or runaway length");

        TASK_CHECK(t);

        if (t->prev != prev)
            panic("sched_queue_verify: prev/next cross-link broken");

        if (t->pid == PID_IDLE)
            found_idle = true;

        prev = t;
        t = t->next;
    }

    if (!found_idle)
        panic("sched_queue_verify: idle_task not in queue");
}

_Static_assert(TASK_STACK_SIZE % PAGE_SIZE == 0,
               "TASK_STACK_SIZE must be a multiple of PAGE_SIZE");

/* Magic value written to the bottom word of every kernel stack.
 * Checked in kstack_free and dump_trap_frame to detect overflow. */
#define KSTACK_CANARY  0xDEADC0DEu

/* Allocate a kernel stack with a guard page immediately below it.
 * Layout: [guard page (unmapped)] [TASK_STACK_SIZE bytes (usable)]
 * Returns a pointer to the first usable byte (i.e. stack_base).
 * The guard page sits at stack_base - PAGE_SIZE; overflowing the stack
 * triggers a page fault rather than silently corrupting adjacent data.
 * A canary word is written at stack_base[0] to detect near-overflow. */
static uint8_t *kstack_alloc(void)
{
    uint32_t total_pages = (TASK_STACK_SIZE / PAGE_SIZE) + 1; /* +1 for guard */
    uint8_t *alloc = (uint8_t *)vmm_alloc_pages(total_pages);
    if (!alloc)
        return NULL;
    /* Unmap the first page to make it a guard page */
    vmm_unmap_page((uint32_t)alloc);
    uint8_t *base = alloc + PAGE_SIZE;
    /* Write canary at the bottom of the usable stack */
    *(uint32_t *)base = KSTACK_CANARY;
    DBGK("kstack_alloc: base=0x%lx canary=0x%lx\n", (uint32_t)base, *(uint32_t *)base);
    return base;
}

/* Free a kernel stack previously allocated with kstack_alloc.
 * stack_base is the value returned by kstack_alloc (first usable byte).
 * vmm_free_pages skips pmm_free_frame for unmapped pages (phys == 0),
 * so passing the guard page's virtual address is safe. */
static void kstack_free(uint8_t *stack_base)
{
    if (!stack_base)
        return;
    if (*(uint32_t *)stack_base != KSTACK_CANARY)
        printk("WARN: kernel stack canary overwritten at 0x%lx — stack overflow!\n",
               (uint32_t)stack_base);
    uint32_t total_pages = (TASK_STACK_SIZE / PAGE_SIZE) + 1;
    vmm_free_pages((void *)((uint32_t)stack_base - PAGE_SIZE), total_pages);
}

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
    unsigned char *stack = kstack_alloc();
    if (stack == NULL)
        panic("setup_task_stack: kstack_alloc failed");

    t->stack_base = (unsigned int)stack;
    t->stack_hwm  = (unsigned int)(stack + TASK_STACK_SIZE); /* start at top, grows down */

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
    TASK_CHECK(new_task);
    TASK_CHECK(new_next);

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

    sched_queue_verify();
}

void create_task(struct task *t, char *name, int prio, void (*entry)(void))
{
    if (prio < SCHED_LEVEL_MIN || prio > SCHED_LEVEL_MAX)
        panic("requested priority out of range");

    /* Disable interrupts while modifying the scheduler queue, since the
     * timer interrupt calls sched() which reads sched_queue. */
    disable_interrupts();

    t->magic = TASK_MAGIC;
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
    t->cwd[0] = '/'; t->cwd[1] = '\0';

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
    sched_queue_verify();

out:
    if (sched_queue == NULL)
        panic("BUG: head of sched_queue is empty");

    DBGK("new_task: %s pid=%d prio=%d\n", name, t->pid, prio);
}

/* Build a synthetic trap frame for a user-mode task. When trapret runs iret,
 * the CPU sees ring 3 CS and pops SS:ESP from the frame, switching to the
 * user stack. entry_point is the virtual address to begin execution at.
 * user_sp is the initial user-mode stack pointer (set up by elf_load). */
static void setup_user_task_stack(struct task *t, uint32_t entry_point, uint32_t user_sp)
{
    unsigned char *stack = kstack_alloc();
    if (stack == NULL)
        panic("setup_user_task_stack: kstack_alloc failed");

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

    t->magic = TASK_MAGIC;
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
    t->cwd[0] = '/'; t->cwd[1] = '\0';

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
    sched_queue_verify();

out:
    if (sched_queue == NULL)
        panic("BUG: head of sched_queue is empty");

    DBGK("new_user_task: %s pid=%d prio=%d\n", name, t->pid, prio);
}

/* Helper to insert a task into the scheduler queue by priority */
static void insert_task_sorted(struct task *t, int prio)
{
    TASK_CHECK(t);

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
    t->next = NULL;
    sched_queue_verify();
}

void create_elf_task(struct task *t, char *name, int prio,
                     const void *elf_data, unsigned int elf_size)
{
    if (prio < SCHED_LEVEL_MIN || prio > SCHED_LEVEL_MAX)
        panic("requested priority out of range");

    disable_interrupts();

    t->magic = TASK_MAGIC;
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
    t->cwd[0] = '/'; t->cwd[1] = '\0';

    /* Load the ELF into the task's address space (also allocates stack) */
    uint32_t user_sp = 0;
    uint32_t entry_point = elf_load(elf_data, elf_size, t->page_dir_phys,
                                    0, NULL, &user_sp, &t->brk);
    if (entry_point == 0)
        panic("create_elf_task: ELF load failed");

    setup_user_task_stack(t, entry_point, user_sp);
    insert_task_sorted(t, prio);

    if (sched_queue == NULL)
        panic("BUG: head of sched_queue is empty");

    DBGK("new_elf_task: %s pid=%d entry=0x%lx prio=%d\n",
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
        uint8_t *p = stale_stack;
        stale_stack = NULL;
        kstack_free(p);
    }
}

/* sched_saved_esp is declared in traps.S (.bss); expose for sched_check_frame */
extern uint32_t sched_saved_esp;


void (*task_death_hook)(uint32_t pid) = NULL;

void task_kill(struct task *t)
{
    DBGK("task_kill: killing pid %d (%s), running_task=%d\n", t->pid, t->name, running_task->pid);
    if (t->pid == PID_IDLE)
        panic("task_kill: cannot kill idle task");

    /* Close all open file descriptors so pipe refcounts are decremented.
     * Must happen before the task is unlinked from the scheduler queue. */
    for (int _i = 0; _i < VFS_FD_MAX; _i++) {
        if (t->fds[_i].open) {
            struct vfs_node *_n = t->fds[_i].node;
            t->fds[_i].open = false;
            t->fds[_i].node = NULL;
            if (_n) pipe_notify_close(_n);
        }
    }

    /* Disable interrupts while manipulating the scheduler queue.
     * Callers that already have interrupts disabled (e.g. sys_kill, the
     * compositor self-kill path) calling disable_interrupts() again is
     * harmless on this single-core x86 kernel. */
    disable_interrupts();

    TASK_CHECK(t);

    /* unlink from scheduler queue */
    if (t->prev != NULL)
        t->prev->next = t->next;
    else
        sched_queue = t->next;

    if (t->next != NULL)
        t->next->prev = t->prev;

    /* Poison magic so any subsequent access to this struct is caught */
    t->magic = TASK_MAGIC_DEAD;

    sched_queue_verify();

    /* notify subsystems (e.g. GUI) that this pid is dying */
    if (task_death_hook)
        task_death_hook(t->pid);

    /* Save pid and stack base before respawn_init() overwrites task_init */
    unsigned int dead_pid = t->pid;
    uint32_t dead_stack_base = t->stack_base;

    /* destroy task's page directory (frees user page tables and frames) */
    if (t->page_dir_phys != vmm_get_kernel_page_dir())
        vmm_destroy_page_dir(t->page_dir_phys);

    /* if the killed task is init, respawn it immediately.
     * respawn_init() memsets &task_init and creates a fresh task, so all
     * fields of *t (including pid and stack_base) are overwritten. Use the
     * saved dead_pid / dead_stack_base from here on — never t->pid etc. */
    if (t == &task_init)
        respawn_init();

    /* Wake any task waiting for this pid before killing children,
     * so waiters are notified even if a child kill doesn't return. */
    DBGK("task_kill: waking waiters for pid %d\n", dead_pid);
    struct task *waiter = sched_queue;
    while (waiter) {
        if (waiter->wait_pid == (int)dead_pid) {
            DBGK("task_kill: waking pid %d (was waiting for %d)\n", waiter->pid, dead_pid);
            waiter->wait_pid    = -1;
            waiter->wait_done   = true;
            waiter->exit_status = t->exit_status;
            waiter->prio_d      = waiter->prio_s;
        }
        waiter = waiter->next;
    }

    /* Kill all direct children. Collect first, safe against queue mutation.
     * If a child is running_task its task_kill won't return — kill it last. */
    struct task *children[64];
    int nchildren = 0;
    struct task *c = sched_queue;
    while (c && nchildren < 64) {
        if (c->ppid == dead_pid)
            children[nchildren++] = c;
        c = c->next;
    }
    DBGK("task_kill: pid %d has %d children\n", dead_pid, nchildren);
    struct task *self_child = NULL;
    for (int i = 0; i < nchildren; i++) {
        if (children[i] == running_task)
            self_child = children[i];
        else
            task_kill(children[i]);
    }
    if (self_child)
        task_kill(self_child); /* does not return */

    /* if we killed the running task, force a context switch.
     * We pick a new task and jump directly to its saved state,
     * bypassing the normal scheduler path since the current
     * trap frame belongs to the dead task. */
    DBGK("task_kill: pid %d done, t==running_task: %d\n", dead_pid, t == running_task);
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
        vmm_switch_page_dir(running_task->page_dir_phys);
        tss_set_kernel_stack(running_task->stack_base + TASK_STACK_SIZE);

        /* Queue the old stack for freeing — traps.S will call
         * sched_free_stale_stack() after switching to the new ESP.
         * Use dead_stack_base (saved before respawn_init may have
         * overwritten t->stack_base with the new init's stack). */
        stale_stack = (void *)dead_stack_base;

        DBGK("task_kill: self-kill pid %d, jumping to pid %d esp=0x%lx\n",
             dead_pid, running_task->pid, (unsigned long)running_task->esp);
        asm volatile(
            "movl %0, %%esp\n\t"
            "jmp sched_enter_trapret\n\t"
            :
            : "r"(running_task->esp)
        );
        __builtin_unreachable();
    }

    /* Killed a different task — free its stack.
     * Do NOT re-enable interrupts here: callers (sys_kill, cascade) may hold
     * interrupts disabled across multiple task_kill calls, and enabling mid-
     * cascade would let the scheduler switch away from the caller's stack.
     * Use dead_stack_base (saved before respawn_init may have overwritten t). */
    if (dead_stack_base != 0)
        kstack_free((uint8_t *)dead_stack_base);
}

struct task *task_fork(struct trap_frame *frame)
{
    struct task *child = (struct task *)kmalloc(sizeof(struct task));
    if (!child) {
        printk("fork: out of memory\n");
        return NULL;
    }

    disable_interrupts();

    TASK_CHECK(running_task);

    /* Copy most fields from parent */
    *child = *running_task;

    child->pid     = ++last_pid;
    child->ppid    = running_task->pid;
    child->created = timekeeper.uptime_ms;
    child->wait_pid  = -1;
    child->wait_done = false;
    child->prev   = NULL;
    child->next   = NULL;

    /* Allocate the child's kernel stack BEFORE cloning the page directory,
     * so the new slab mapping is in the kernel page directory at clone time
     * and the child's copy inherits it. */
    uint8_t *kstack = kstack_alloc();
    if (!kstack) {
        kfree(child);
        enable_interrupts();
        printk("fork: out of memory for kernel stack\n");
        return NULL;
    }

    /* Deep-copy the address space (kernel page dir now includes kstack mapping) */
    child->page_dir_phys = vmm_clone_page_dir(running_task->page_dir_phys);
    uint32_t dbg_kstack_top = (uint32_t)kstack + TASK_STACK_SIZE - PAGE_SIZE;
    uint32_t dbg_pdi = dbg_kstack_top >> 22;
    uint32_t dbg_pti = (dbg_kstack_top >> 12) & 0x3FF;
    uint32_t *dbg_child_dir = (uint32_t *)child->page_dir_phys;
    uint32_t dbg_child_pde  = dbg_child_dir[dbg_pdi];
    uint32_t dbg_kern_pde   = ((uint32_t *)vmm_get_kernel_page_dir())[dbg_pdi];
    DBGK("task_fork: kstack_top=0x%lx pdi=0x%lx child_pde=0x%lx kern_pde=0x%lx shared=%d\n",
         dbg_kstack_top, dbg_pdi, dbg_child_pde, dbg_kern_pde,
         (dbg_child_pde & 0xFFFFF000) == (dbg_kern_pde & 0xFFFFF000));
    if (dbg_child_pde & 1) {
        uint32_t *dbg_pt = (uint32_t *)(dbg_child_pde & 0xFFFFF000);
        DBGK("task_fork: child PTE[%ld]=0x%lx\n", dbg_pti, dbg_pt[dbg_pti]);
    }

    uint8_t *parent_kstack = (uint8_t *)running_task->stack_base;

    /* Copy parent kstack to child page-by-page via physical addresses.
     * kstack and parent_kstack may share the same virtual address (the vmm
     * free-list can reuse a range freed by a previous task), so a plain
     * memcpy under the parent CR3 would be a no-op.  vmm_get_phys resolves
     * through the kernel page directory (which always has the correct
     * mappings), and vmm_kmap gives us a distinct virtual window onto each
     * child physical frame so the write actually lands on the right page. */
    for (uint32_t i = 0; i < TASK_STACK_SIZE / PAGE_SIZE; i++) {
        uint32_t child_phys = vmm_get_phys((uint32_t)kstack + i * PAGE_SIZE);
        void *kp = vmm_kmap(child_phys);
        memcpy(kp, parent_kstack + i * PAGE_SIZE, PAGE_SIZE);
        vmm_kunmap(kp);
    }
    DBGK("task_fork: kstack copy done child_virt=0x%lx parent_virt=0x%lx alias=%ld\n",
         (uint32_t)kstack, (uint32_t)parent_kstack,
         (uint32_t)(kstack == parent_kstack));

    child->stack_base = (unsigned int)kstack;
    child->stack_hwm  = (unsigned int)(kstack + TASK_STACK_SIZE);

    /* Use the current syscall trap frame as the saved ESP.
     * running_task->esp is only updated on context switch, so it may be
     * stale. The frame pointer IS the current kernel stack position. */
    DBGK("task_fork: frame=0x%lx frame->ds=0x%lx frame->eip=0x%lx frame->cs=0x%lx\n",
         (unsigned long)frame, (unsigned long)frame->ds, (unsigned long)frame->eip, (unsigned long)frame->cs);
    uint32_t parent_frame_esp = (uint32_t)frame;
    uint32_t stack_offset = parent_frame_esp - running_task->stack_base;
    child->esp = child->stack_base + stack_offset;

    /* Set child's return value (EAX in trap frame) to 0.
     * The trap frame sits on the child's kstack at stack_offset from base.
     * child->esp may alias the parent's virtual address (same virt, different
     * phys), so we must write through a kmap window, not through the pointer. */
    uint32_t child_frame_phys = vmm_get_phys(child->esp & ~0xFFFu) | (child->esp & 0xFFFu);
    void *child_frame_kp = vmm_kmap(child_frame_phys & ~0xFFFu);
    struct trap_frame *child_frame = (struct trap_frame *)((uint8_t *)child_frame_kp + (child->esp & 0xFFFu));
    child_frame->eax = 0;
    DBGK("task_fork: child_frame phys=0x%lx ds=0x%lx eip=0x%lx cs=0x%lx\n",
         child_frame_phys, (unsigned long)child_frame->ds,
         (unsigned long)child_frame->eip, (unsigned long)child_frame->cs);
    vmm_kunmap(child_frame_kp);

    /* Insert child into scheduler after parent */
    insert_task_sorted(child, child->prio_s);

    enable_interrupts();

    DBGK("fork: pid %d -> child pid %d, child->esp=0x%lx stack_base=0x%lx offset=%ld\n",
         running_task->pid, child->pid, (unsigned long)child->esp, (unsigned long)child->stack_base,
         (unsigned long)(child->esp - child->stack_base));
    return child;
}

int task_exec(const void *elf_data, uint32_t elf_size, struct trap_frame *frame,
              int argc, const char *const *argv)
{
    /* Capture task name and cmdline from argv before the address space is
     * replaced. argv pointers are user-space, valid in the current page dir. */
    if (argc > 0 && argv[0]) {
        const char *base = argv[0];
        for (const char *p = argv[0]; *p; p++)
            if (*p == '/')
                base = p + 1;
        strncpy(running_task->name, base, sizeof(running_task->name) - 1);
        running_task->name[sizeof(running_task->name) - 1] = '\0';

        /* Build cmdline: argv[0..argc-1] joined by spaces */
        uint32_t pos = 0;
        for (int i = 0; i < argc && argv[i]; i++) {
            if (i > 0 && pos < sizeof(running_task->cmdline) - 1)
                running_task->cmdline[pos++] = ' ';
            const char *src = (i == 0) ? base : argv[i];
            for (; *src && pos < sizeof(running_task->cmdline) - 1; src++)
                running_task->cmdline[pos++] = *src;
        }
        running_task->cmdline[pos] = '\0';
    }

    /* Create a new page directory for the new image */
    uint32_t new_dir = vmm_create_page_dir();

    uint32_t new_sp = 0;
    uint32_t new_brk = 0;
    uint32_t entry = elf_load(elf_data, elf_size, new_dir, argc, argv, &new_sp, &new_brk);
    if (entry == 0) {
        vmm_destroy_page_dir(new_dir);
        return -1;
    }

    /* Free the old address space */
    uint32_t old_dir = running_task->page_dir_phys;

    /* Switch to new address space before destroying the old one */
    running_task->page_dir_phys = new_dir;
    running_task->brk           = new_brk;
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
    idle_task.magic = TASK_MAGIC;
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
