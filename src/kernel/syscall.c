/* Syscall dispatch - handles int 0x80 from user mode.
 *
 * Convention: syscall number in EAX, arguments in EBX, ECX, EDX.
 * Return value placed in EAX of the trap frame.
 */

#include "kernel/kernel.h"
#include "kernel/syscall.h"
#include "kernel/task.h"
#include "kernel/sched.h"
#include "kernel/heap.h"
#include "kernel/gui.h"
#include "boot/multiboot.h"
#include "cpu/traps.h"
#include "cpu/x86.h"
#include "drivers/keyboard.h"
#include "drivers/mouse.h"
#include "drivers/fbdev.h"

static int sys_nop(void)
{
    return 0;
}

static int sys_write(struct trap_frame *frame)
{
    /* EBX = buffer pointer, ECX = length */
    const char *buf = (const char *)frame->ebx;
    unsigned int len = frame->ecx;

    /* Basic validation: reject null pointers and unreasonable lengths */
    if (buf == NULL || len > 4096)
        return -1;

    /* Print character by character (safe, no format string issues) */
    for (unsigned int i = 0; i < len; i++)
        printk("%c", buf[i]);

    return (int)len;
}

static int sys_exit(struct trap_frame *frame)
{
    printk("exit: running='%s'(pid %d) frame=0x%lx\n",
           running_task->name, running_task->pid, (uint32_t)frame);
    printk("  eax=%ld eip=0x%lx cs=0x%lx eflags=0x%lx\n",
           frame->eax, frame->eip, frame->cs, frame->eflags);
    printk("  magic=0x%lx (expect 0x%lx) trapno=%ld err=%ld\n",
           frame->magic, (uint32_t)TRAP_MAGIC, frame->trapno, frame->err);
    printk("  ebp=0x%lx esp=0x%lx ebx=0x%lx uesp=0x%lx\n",
           frame->ebp, frame->esp, frame->ebx, frame->uesp);
    /* dump all tasks */
    struct task *_t = sched_queue;
    while (_t) {
        printk("  task pid=%d '%s' stack=0x%lx..0x%lx saved_esp=0x%lx\n",
               _t->pid, _t->name, _t->stack_base,
               _t->stack_base + 4096, _t->esp);
        _t = _t->next;
    }
    while (1)
        asm volatile("hlt");
    return 0; /* unreachable */
}

static int sys_yield(void)
{
    enable_interrupts();
    asm volatile("hlt");
    disable_interrupts();
    return 0;
}

static int sys_read(struct trap_frame *frame)
{
    char *buf = (char *)frame->ebx;
    unsigned int len = frame->ecx;

    if (buf == NULL || len > 4096)
        return -1;

    /* Loop: consume scroll sentinel keys without returning them to user space */
    for (;;) {
        int n = keyboard_read(buf, len);

        /* Filter out scroll sentinels and handle them in-kernel */
        int out = 0;
        for (int i = 0; i < n; i++) {
            if (buf[i] == KEY_PGUP) {
                fbdev_scroll_up();
            } else if (buf[i] == KEY_PGDN) {
                fbdev_scroll_down();
            } else {
                buf[out++] = buf[i];
            }
        }

        if (out > 0)
            return out;

        /* Buffer empty - briefly enable interrupts so pending IRQs
         * (keyboard, timer) can fire before we return to userspace. */
        enable_interrupts();
        asm volatile("hlt");
        disable_interrupts();
    }
}

static int sys_uptime(void)
{
    return (int)timekeeper.uptime_ms;
}

static int sys_getpid(void)
{
    return (int)running_task->pid;
}

static int sys_halt(void)
{
    printk("system halted by pid %d\n", running_task->pid);
    extern void cpu_halt(void);
    cpu_halt();
    return 0; /* unreachable */
}

static int sys_ps(void)
{
    struct task *t = sched_queue;
    printk("PID  PRIO     NAME\n");
    while (t != NULL) {
        printk("%-4d %-8d %s\n", t->pid, t->prio_s, t->name);
        t = t->next;
    }
    return 0;
}

/* ── GUI syscall helpers ─────────────────────────────────────────────────── */

/* Read a uint32_t from user stack at offset bytes past uesp.
 * Extra args pushed before int $0x80 are at [uesp+0], [uesp+4], [uesp+8]...
 * so the first extra arg is at offset 0, second at 4, third at 8. */
static uint32_t user_stack_arg(struct trap_frame *frame, unsigned int offset)
{
    uint32_t *usp = (uint32_t *)frame->uesp;
    return usp[offset / 4];
}

static int sys_win_create(struct trap_frame *frame)
{
    /* EBX=x, ECX=y, EDX=w, [uesp+0]=h, [uesp+4]=title_ptr */
    int32_t  x        = (int32_t)frame->ebx;
    int32_t  y        = (int32_t)frame->ecx;
    uint32_t w        = frame->edx;
    uint32_t h        = user_stack_arg(frame, 0);
    const char *title = (const char *)user_stack_arg(frame, 4);

    if (!title || w == 0 || h == 0) return -1;
    return gui_window_create(x, y, w, h, title);
}

static int sys_win_destroy(struct trap_frame *frame)
{
    gui_window_destroy((int)frame->ebx);
    return 0;
}

static int sys_win_fill_rect(struct trap_frame *frame)
{
    /* EBX=id, ECX=x, EDX=y, [uesp+0]=w, [uesp+4]=h, [uesp+8]=color */
    int      id    = (int)frame->ebx;
    uint32_t x     = frame->ecx;
    uint32_t y     = frame->edx;
    uint32_t w     = user_stack_arg(frame, 0);
    uint32_t h     = user_stack_arg(frame, 4);
    uint32_t color = user_stack_arg(frame, 8);
    gui_window_fill_rect(id, x, y, w, h, color);
    return 0;
}

static int sys_win_draw_rect(struct trap_frame *frame)
{
    /* EBX=id, ECX=x, EDX=y, [uesp+0]=w, [uesp+4]=h, [uesp+8]=color */
    int      id    = (int)frame->ebx;
    uint32_t x     = frame->ecx;
    uint32_t y     = frame->edx;
    uint32_t w     = user_stack_arg(frame, 0);
    uint32_t h     = user_stack_arg(frame, 4);
    uint32_t color = user_stack_arg(frame, 8);
    gui_window_draw_rect(id, x, y, w, h, color);
    return 0;
}

static int sys_win_draw_text(struct trap_frame *frame)
{
    /* EBX=id, ECX=x, EDX=y, [uesp+0]=str_ptr, [uesp+4]=fgcolor, [uesp+8]=bgcolor */
    int         id      = (int)frame->ebx;
    uint32_t    x       = frame->ecx;
    uint32_t    y       = frame->edx;
    const char *str     = (const char *)user_stack_arg(frame, 0);
    uint32_t    fgcolor = user_stack_arg(frame, 4);
    uint32_t    bgcolor = user_stack_arg(frame, 8);

    if (!str) return -1;
    gui_window_draw_text(id, x, y, str, fgcolor, bgcolor);
    return 0;
}

static int sys_win_flip(struct trap_frame *frame)
{
    gui_window_flip((int)frame->ebx);
    return 0;
}

static int sys_win_poll_event(struct trap_frame *frame)
{
    /* EBX=id, ECX=ptr to struct gui_event in user space */
    int id = (int)frame->ebx;
    struct gui_event *ev = (struct gui_event *)frame->ecx;
    if (!ev) return -1;
    return gui_window_poll_event(id, ev);
}

static int sys_mouse_get(struct trap_frame *frame)
{
    /* EBX=ptr to struct { uint32_t x, y; uint8_t buttons; } in user space */
    uint32_t *buf = (uint32_t *)frame->ebx;
    if (!buf) return -1;
    buf[0] = mouse_x;
    buf[1] = mouse_y;
    buf[2] = (uint32_t)mouse_buttons;
    return 1;
}

static int sys_kill(struct trap_frame *frame)
{
    unsigned int target_pid = frame->ebx;

    if (target_pid == PID_IDLE) {
        printk("kill: cannot kill idle task\n");
        return -1;
    }

    /* Walk scheduler queue to find the task */
    struct task *t = sched_queue;
    while (t) {
        if (t->pid == target_pid) {
            printk("kill: terminating pid %d (%s)\n", t->pid, t->name);
            disable_interrupts();
            task_kill(t);
            /* task_kill() only returns if we killed a different task */
            enable_interrupts();
            return 0;
        }
        t = t->next;
    }

    printk("kill: pid %d not found\n", target_pid);
    return -1;
}

static int sys_exec(struct trap_frame *frame)
{
    unsigned int mod_idx = frame->ebx;

    if (!mb_info || mod_idx >= mb_info->mods_count) {
        printk("exec: invalid module index %d (mods_count=%ld)\n",
               mod_idx, mb_info ? mb_info->mods_count : 0);
        return -1;
    }

    struct multiboot_mod_entry *mods =
        (struct multiboot_mod_entry *)mb_info->mods_addr;
    struct multiboot_mod_entry *mod = &mods[mod_idx];
    uint32_t mod_size = mod->mod_end - mod->mod_start;

    /* Allocate a task struct from the kernel heap */
    struct task *t = (struct task *)kmalloc(sizeof(struct task));
    if (!t) {
        printk("exec: out of memory\n");
        return -1;
    }

    create_elf_task(t, "user_task", 5,
                    (const void *)mod->mod_start, mod_size);

    printk("exec: spawned module %d as pid %d\n", mod_idx, t->pid);
    return (int)t->pid;
}

/* ── dispatch ─────────────────────────────────────────────────────────────── */

void syscall_handler(struct trap_frame *frame)
{
    int ret;

    switch (frame->eax) {
    case SYS_NOP:
        ret = sys_nop();
        break;
    case SYS_WRITE:
        ret = sys_write(frame);
        break;
    case SYS_EXIT:
        ret = sys_exit(frame);
        break;
    case SYS_READ:
        ret = sys_read(frame);
        break;
    case SYS_UPTIME:
        ret = sys_uptime();
        break;
    case SYS_GETPID:
        ret = sys_getpid();
        break;
    case SYS_HALT:
        ret = sys_halt();
        break;
    case SYS_PS:
        ret = sys_ps();
        break;
    case SYS_WIN_CREATE:
        ret = sys_win_create(frame);
        break;
    case SYS_WIN_DESTROY:
        ret = sys_win_destroy(frame);
        break;
    case SYS_WIN_FILL_RECT:
        ret = sys_win_fill_rect(frame);
        break;
    case SYS_WIN_DRAW_RECT:
        ret = sys_win_draw_rect(frame);
        break;
    case SYS_WIN_DRAW_TEXT:
        ret = sys_win_draw_text(frame);
        break;
    case SYS_WIN_FLIP:
        ret = sys_win_flip(frame);
        break;
    case SYS_WIN_POLL_EVENT:
        ret = sys_win_poll_event(frame);
        break;
    case SYS_MOUSE_GET:
        ret = sys_mouse_get(frame);
        break;
    case SYS_EXEC:
        ret = sys_exec(frame);
        break;
    case SYS_KILL:
        ret = sys_kill(frame);
        break;
    case SYS_YIELD:
        ret = sys_yield();
        break;
    default:
        printk("unknown syscall %d from pid %d\n",
               frame->eax, running_task->pid);
        ret = -1;
        break;
    }

    /* Return value goes in EAX */
    frame->eax = (unsigned int)ret;
}
