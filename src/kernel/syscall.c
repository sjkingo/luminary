/* Syscall dispatch - handles int 0x80 from user mode.
 *
 * Convention: syscall number in EAX, arguments in EBX, ECX, EDX.
 * Return value placed in EAX of the trap frame.
 */

#include <stdbool.h>
#include "kernel/kernel.h"
#include "kernel/syscall.h"
#include "kernel/task.h"
#include "kernel/sched.h"
#include "kernel/heap.h"
#include "kernel/gui.h"
#include "kernel/vmm.h"
#include "kernel/vfs.h"
#include "kernel/initrd.h"
#include "boot/multiboot.h"
#include "cpu/traps.h"
#include "cpu/x86.h"
#include "drivers/keyboard.h"
#include "drivers/mouse.h"
#include "drivers/fbdev.h"

/* Validate that a user-supplied pointer range lies entirely within user space.
 * Pass len=0 to check only the start address (e.g. for string pointers where
 * the length is unknown). Returns true if the range is acceptable. */
static inline bool user_access_ok(const void *ptr, uint32_t len)
{
    uint32_t addr = (uint32_t)ptr;
    if (addr < USER_SPACE_START) return false;
    if (addr >= USER_SPACE_END)  return false;
    if (len > 1 && (addr + len - 1) >= USER_SPACE_END) return false;
    return true;
}

static int sys_nop(void)
{
    return 0;
}

static int sys_write(struct trap_frame *frame)
{
    /* EBX = buffer pointer, ECX = length */
    const char *buf = (const char *)frame->ebx;
    unsigned int len = frame->ecx;

    if (!user_access_ok(buf, len) || len > 4096)
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
           (uint32_t)frame->eax, (uint32_t)frame->eip,
           (uint32_t)frame->cs, (uint32_t)frame->eflags);
    printk("  magic=0x%lx (expect 0x%lx) trapno=%ld err=%ld\n",
           (uint32_t)frame->magic, (uint32_t)TRAP_MAGIC,
           (uint32_t)frame->trapno, (uint32_t)frame->err);
    printk("  ebp=0x%lx esp=0x%lx ebx=0x%lx uesp=0x%lx\n",
           (uint32_t)frame->ebp, (uint32_t)frame->esp,
           (uint32_t)frame->ebx, (uint32_t)frame->uesp);
    /* dump all tasks */
    struct task *_t = sched_queue;
    while (_t) {
        printk("  task pid=%d '%s' stack=0x%lx..0x%lx saved_esp=0x%lx\n",
               _t->pid, _t->name, (uint32_t)_t->stack_base,
               (uint32_t)(_t->stack_base + 4096), (uint32_t)_t->esp);
        _t = _t->next;
    }
    while (1)
        asm volatile("hlt");
    return 0; /* unreachable */
}

static int sys_win_get_size(struct trap_frame *frame)
{
    int id       = (int)frame->ebx;
    uint32_t *pw = (uint32_t *)frame->ecx;
    uint32_t *ph = (uint32_t *)frame->edx;
    if (!user_access_ok(pw, sizeof(uint32_t)) ||
        !user_access_ok(ph, sizeof(uint32_t))) return -1;
    return gui_window_get_size(id, pw, ph);
}

static int sys_read_nb(struct trap_frame *frame)
{
    char *buf = (char *)frame->ebx;
    unsigned int len = frame->ecx;
    if (!user_access_ok(buf, len) || len > 4096) return -1;
    return keyboard_read(buf, len);
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

    if (!user_access_ok(buf, len) || len > 4096)
        return -1;

    /* Loop: consume scroll sentinel keys without returning them to user space */
    for (;;) {
        /* While GUI windows are open, keyboard is owned by the compositor.
         * Block here without consuming any input. */
        if (gui_has_windows()) {
            enable_interrupts();
            asm volatile("hlt");
            disable_interrupts();
            continue;
        }

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

    if (!user_access_ok(title, 1) || w == 0 || h == 0) return -1;
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

    if (!user_access_ok(str, 1)) return -1;
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
    if (!user_access_ok(ev, sizeof(struct gui_event))) return -1;
    return gui_window_poll_event(id, ev);
}

static int sys_mouse_get(struct trap_frame *frame)
{
    /* EBX=ptr to struct { uint32_t x, y; uint8_t buttons; } in user space */
    uint32_t *buf = (uint32_t *)frame->ebx;
    if (!user_access_ok(buf, 3 * sizeof(uint32_t))) return -1;
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

static int sys_spawn(struct trap_frame *frame)
{
    /* EBX = pointer to path string in user space */
    const char *path = (const char *)frame->ebx;
    if (!user_access_ok(path, 1)) return -1;

    uint32_t elf_size = 0;
    const void *elf_data = initrd_get_file(path, &elf_size);
    if (!elf_data) {
        printk("spawn: '%s' not found in VFS\n", path);
        return -1;
    }

    struct task *t = (struct task *)kmalloc(sizeof(struct task));
    if (!t) {
        printk("spawn: out of memory\n");
        return -1;
    }

    /* Use the basename as the task name */
    const char *name = path;
    for (const char *s = path; *s; s++)
        if (*s == '/') name = s + 1;

    create_elf_task(t, (char *)name, 5, elf_data, elf_size);
    printk("spawn: spawned '%s' as pid %d\n", name, t->pid);
    return (int)t->pid;
}

static int sys_exec(struct trap_frame *frame)
{
    const char *path = (const char *)frame->ebx;
    uint32_t argv_ptr = frame->ecx;   /* user pointer to char *argv[] (NULL-terminated) */

    if (!user_access_ok(path, 1)) return -1;

    /* Collect argv from user space */
    static const char *kargv[32];
    int argc = 0;

    if (argv_ptr && user_access_ok((void *)argv_ptr, 4)) {
        uint32_t *uargv = (uint32_t *)argv_ptr;
        while (argc < 31 && uargv[argc] && user_access_ok((void *)uargv[argc], 1)) {
            kargv[argc] = (const char *)uargv[argc];
            argc++;
        }
    }
    kargv[argc] = NULL;

    uint32_t elf_size = 0;
    const void *elf_data = initrd_get_file(path, &elf_size);
    if (!elf_data) {
        printk("exec: '%s' not found\n", path);
        return -1;
    }

    return task_exec(elf_data, elf_size, frame, argc, kargv);
}

static int sys_fork(struct trap_frame *frame)
{
    struct task *child = task_fork(frame);
    if (!child) return -1;
    return (int)child->pid;
}

static int sys_waitpid(struct trap_frame *frame)
{
    int target_pid = (int)frame->ebx;
    if (target_pid <= 0) return -1;

    /* Verify the target is a child of the caller */
    struct task *t = sched_queue;
    bool found = false;
    while (t) {
        if ((int)t->pid == target_pid && (int)t->ppid == (int)running_task->pid) {
            found = true;
            break;
        }
        t = t->next;
    }
    if (!found) {
        /* child may have already exited and been removed */
        if (running_task->wait_done) {
            running_task->wait_done = false;
            return target_pid;
        }
        return -1;
    }

    /* Block until the child exits */
    running_task->wait_pid  = target_pid;
    running_task->wait_done = false;
    running_task->prio_d    = SCHED_LEVEL_SUSP;

    while (!running_task->wait_done) {
        enable_interrupts();
        asm volatile("hlt");
        disable_interrupts();
    }

    running_task->wait_done = false;
    return target_pid;
}

static int sys_exit_task(struct trap_frame *frame)
{
    (void)frame;
    task_kill(running_task);
    __builtin_unreachable();
}

/* ── VFS syscalls ─────────────────────────────────────────────────────────── */

/* Allocate the lowest available fd in the running task. Returns -1 if full. */
static int fd_alloc(struct vfs_node *node)
{
    for (int i = 0; i < VFS_FD_MAX; i++) {
        if (!running_task->fds[i].open) {
            running_task->fds[i].open    = true;
            running_task->fds[i].node    = node;
            running_task->fds[i].offset  = 0;
            running_task->fds[i].dir_idx = 0;
            return i;
        }
    }
    return -1;
}

static int sys_open(struct trap_frame *frame)
{
    const char *path = (const char *)frame->ebx;
    if (!user_access_ok(path, 1)) return -1;

    struct vfs_node *node = vfs_lookup(path);
    if (!node) return -1;

    return fd_alloc(node);
}

static int sys_close(struct trap_frame *frame)
{
    int fd = (int)frame->ebx;
    if (fd < 0 || fd >= VFS_FD_MAX) return -1;
    if (!running_task->fds[fd].open) return -1;
    running_task->fds[fd].open = false;
    running_task->fds[fd].node = NULL;
    return 0;
}

static int sys_read_fd(struct trap_frame *frame)
{
    int      fd  = (int)frame->ebx;
    void    *buf = (void *)frame->ecx;
    uint32_t len = frame->edx;

    if (fd < 0 || fd >= VFS_FD_MAX) return -1;
    if (!running_task->fds[fd].open) return -1;
    if (!user_access_ok(buf, len)) return -1;

    struct vfs_fd *vfd = &running_task->fds[fd];
    if (!(vfd->node->flags & VFS_FILE)) return -1;

    uint32_t n = vfs_read(vfd->node, vfd->offset, len, buf);
    vfd->offset += n;
    return (int)n;
}

static int sys_lseek(struct trap_frame *frame)
{
    int      fd     = (int)frame->ebx;
    uint32_t offset = frame->ecx;
    int      whence = (int)frame->edx;

    if (fd < 0 || fd >= VFS_FD_MAX) return -1;
    if (!running_task->fds[fd].open) return -1;

    struct vfs_fd *vfd = &running_task->fds[fd];
    if (!(vfd->node->flags & VFS_FILE)) return -1;

    uint32_t new_off;
    if (whence == 0)      new_off = offset;               /* SEEK_SET */
    else if (whence == 1) new_off = vfd->offset + offset; /* SEEK_CUR */
    else if (whence == 2) new_off = vfd->node->size + offset; /* SEEK_END */
    else return -1;

    if (new_off > vfd->node->size) new_off = vfd->node->size;
    vfd->offset = new_off;
    return (int)new_off;
}

static int sys_readdir(struct trap_frame *frame)
{
    int fd = (int)frame->ebx;
    struct vfs_dirent *out = (struct vfs_dirent *)frame->ecx;

    if (fd < 0 || fd >= VFS_FD_MAX) return -1;
    if (!running_task->fds[fd].open) return -1;
    if (!user_access_ok(out, sizeof(struct vfs_dirent))) return -1;

    struct vfs_fd *vfd = &running_task->fds[fd];
    if (!(vfd->node->flags & VFS_DIR)) return -1;

    struct vfs_node *child = vfs_readdir(vfd->node, vfd->dir_idx);
    if (!child) return 0; /* end of directory */

    vfd->dir_idx++;
    /* fill dirent */
    uint32_t nlen = (uint32_t)strlen(child->name);
    if (nlen >= VFS_NAME_MAX) nlen = VFS_NAME_MAX - 1;
    memcpy(out->name, child->name, nlen);
    out->name[nlen] = '\0';
    out->inode = child->inode;
    out->type  = child->flags;
    return 1;
}

static int sys_stat(struct trap_frame *frame)
{
    const char *path = (const char *)frame->ebx;
    struct vfs_stat *out = (struct vfs_stat *)frame->ecx;

    if (!user_access_ok(path, 1)) return -1;
    if (!user_access_ok(out, sizeof(struct vfs_stat))) return -1;

    return vfs_stat(path, out);
}

static int sys_mount(struct trap_frame *frame)
{
    (void)frame;
    /* Print VFS mount information to the kernel console */
    printk("mount:\n");
    printk("  / (initrd, cpio, ro)\n");
    return 0;
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
    case SYS_SPAWN:
        ret = sys_spawn(frame);
        break;
    case SYS_KILL:
        ret = sys_kill(frame);
        break;
    case SYS_YIELD:
        ret = sys_yield();
        break;
    case SYS_WIN_GET_SIZE:
        ret = sys_win_get_size(frame);
        break;
    case SYS_READ_NB:
        ret = sys_read_nb(frame);
        break;
    case SYS_OPEN:
        ret = sys_open(frame);
        break;
    case SYS_CLOSE:
        ret = sys_close(frame);
        break;
    case SYS_READ_FD:
        ret = sys_read_fd(frame);
        break;
    case SYS_LSEEK:
        ret = sys_lseek(frame);
        break;
    case SYS_READDIR:
        ret = sys_readdir(frame);
        break;
    case SYS_STAT:
        ret = sys_stat(frame);
        break;
    case SYS_MOUNT:
        ret = sys_mount(frame);
        break;
    case SYS_EXEC:
        ret = sys_exec(frame);
        break;
    case SYS_FORK:
        ret = sys_fork(frame);
        break;
    case SYS_WAITPID:
        ret = sys_waitpid(frame);
        break;
    case SYS_EXIT_TASK:
        ret = sys_exit_task(frame);
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
