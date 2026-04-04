/* Syscall dispatch - handles int 0x80 from user mode.
 *
 * Convention: syscall number in EAX, arguments in EBX, ECX, EDX.
 * Return value placed in EAX of the trap frame.
 */

#include <stdbool.h>
#include <string.h>
#include "kernel/kernel.h"
#include "kernel/dev.h"
#include "kernel/pipe.h"
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
    if (len > 1 && len > USER_SPACE_END - addr) return false;
    return true;
}

static int sys_nop(void)
{
    return 0;
}

static int sys_write(struct trap_frame *frame)
{
    /* EBX = fd, ECX = buffer pointer, EDX = length */
    int fd = (int)frame->ebx;
    const char *buf = (const char *)frame->ecx;
    unsigned int len = frame->edx;

    if (fd < 0 || fd >= VFS_FD_MAX) return -1;
    if (!user_access_ok(buf, len) || len > 65536) return -1;

    struct vfs_fd *vfd = &running_task->fds[fd];
    if (!vfd->open || !vfd->node) return -1;

    if (vfd->node->flags & VFS_CHARDEV) {
        vfs_write(vfd->node, 0, len, buf);
        return (int)len;
    }

    if ((vfd->node->flags & VFS_FILE) && vfd->node->writable) {
        uint32_t off = vfd->append ? vfd->node->size : vfd->offset;
        uint32_t n = vfs_write(vfd->node, off, len, buf);
        vfd->offset = off + n;
        return (int)n;
    }

    return -1;
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


static int sys_yield(void)
{
    enable_interrupts();
    asm volatile("hlt");
    disable_interrupts();
    return 0;
}

static int sys_read(struct trap_frame *frame)
{
    /* EBX = fd, ECX = buffer pointer, EDX = length */
    int fd = (int)frame->ebx;
    char *buf = (char *)frame->ecx;
    unsigned int len = frame->edx;

    if (fd < 0 || fd >= VFS_FD_MAX) return -1;
    if (!user_access_ok(buf, len) || len > 4096) return -1;

    struct vfs_fd *vfd = &running_task->fds[fd];
    if (!vfd->open || !vfd->node) return -1;

    if (vfd->node->flags & (VFS_FILE | VFS_CHARDEV)) {
        uint32_t n = vfs_read(vfd->node, vfd->offset, len, buf);
        if (!(vfd->node->flags & VFS_CHARDEV))
            vfd->offset += n;
        return (int)n;
    }

    return -1;
}

static int sys_read_nb(struct trap_frame *frame)
{
    /* Non-blocking read: identical to sys_read but pipe reads return 0
     * immediately if the buffer is empty instead of blocking. */
    int fd = (int)frame->ebx;
    char *buf = (char *)frame->ecx;
    unsigned int len = frame->edx;

    if (fd < 0 || fd >= VFS_FD_MAX) return -1;
    if (!user_access_ok(buf, len) || len > 4096) return -1;

    struct vfs_fd *vfd = &running_task->fds[fd];
    if (!vfd->open || !vfd->node) return -1;

    if (vfd->node->flags & (VFS_FILE | VFS_CHARDEV)) {
        running_task->read_nonblock = true;
        uint32_t n = vfs_read(vfd->node, vfd->offset, len, buf);
        running_task->read_nonblock = false;
        if (!(vfd->node->flags & VFS_CHARDEV))
            vfd->offset += n;
        return (int)n;
    }

    return -1;
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

static int sys_ps(struct trap_frame *frame)
{
    /* EBX = buf, ECX = buflen — format process list into userland buffer */
    char *buf = (char *)frame->ebx;
    unsigned int buflen = (unsigned int)frame->ecx;

    if (!user_access_ok(buf, buflen) || buflen == 0) return -1;

    unsigned int pos = 0;
    char tmp[128];

    /* Header — tab-separated for easy parsing */
    const char *hdr = "PID\tPPID\tPRIO\tTIME\tCMD\n";
    unsigned int i = 0;
    while (hdr[i] && pos < buflen - 1) buf[pos++] = hdr[i++];

    /* Collect tasks into a local array, then emit in PID order */
    struct task *tasks[64];
    int ntasks = 0;
    struct task *t = sched_queue;
    while (t && ntasks < 64) {
        tasks[ntasks++] = t;
        t = t->next;
    }

    /* Insertion sort by pid ascending */
    for (int i = 1; i < ntasks; i++) {
        struct task *key = tasks[i];
        int j = i - 1;
        while (j >= 0 && tasks[j]->pid > key->pid) {
            tasks[j + 1] = tasks[j];
            j--;
        }
        tasks[j + 1] = key;
    }

    for (int i = 0; i < ntasks && pos < buflen - 1; i++) {
        t = tasks[i];
        unsigned int age_s = t->created / 1000;
        int n = sprintf(tmp, "%d\t%d\t%d\t%lu\t%s\n",
                        t->pid, t->ppid, t->prio_s, (unsigned long)age_s, t->name);
        for (int j = 0; j < n && pos < buflen - 1; j++)
            buf[pos++] = tmp[j];
    }
    buf[pos] = '\0';
    return (int)pos;
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

static const char *resolve_path(const char *upath, char *out_buf);

static int sys_exec(struct trap_frame *frame)
{
    char resolved[VFS_PATH_MAX];
    const char *path = resolve_path((const char *)frame->ebx, resolved);
    uint32_t argv_ptr = frame->ecx;   /* user pointer to char *argv[] (NULL-terminated) */

    if (!path) return -1;

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

    int r = task_exec(elf_data, elf_size, frame, argc, kargv);
    if (r == 0)
        cpu_reset_fault_counter();
    return r;
}

static int sys_fork(struct trap_frame *frame)
{
    struct task *child = task_fork(frame);
    if (!child) return -1;

    /* Bump pipe refcounts for every inherited pipe fd */
    for (int i = 0; i < VFS_FD_MAX; i++) {
        if (child->fds[i].open && child->fds[i].node)
            pipe_fork_fd(child->fds[i].node);
    }

    return (int)child->pid;
}

static int sys_waitpid(struct trap_frame *frame)
{
    /* EBX = pid, ECX = &status (int *, may be NULL), EDX = flags */
    int target_pid = (int)frame->ebx;
    int *status    = (int *)frame->ecx;
    int  flags     = (int)frame->edx;

#define WNOHANG 1

    if (target_pid <= 0) return -1;
    if (status && !user_access_ok(status, sizeof(int))) return -1;

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
            if (status) *status = running_task->exit_status;
            return target_pid;
        }
        return -1;
    }

    /* WNOHANG: return immediately if child hasn't exited yet */
    if (flags & WNOHANG) return -1;

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
    if (status) *status = running_task->exit_status;
    return target_pid;
}

static int sys_task_done(struct trap_frame *frame)
{
    /* Returns 1 if pid is no longer in the scheduler queue, 0 if still running */
    unsigned int target_pid = frame->ebx;
    struct task *t = sched_queue;
    while (t) {
        if (t->pid == target_pid) return 0;
        t = t->next;
    }
    return 1;
}

static int sys_exit_task(struct trap_frame *frame)
{
    running_task->exit_status = (int)frame->ebx;
    cpu_reset_fault_counter();
    task_kill(running_task);
    __builtin_unreachable();
}

static int sys_getppid(void)
{
    return (int)running_task->ppid;
}

/* ── VFS syscalls ─────────────────────────────────────────────────────────── */

/* Linux open(2) flag bits (O_CREAT, O_TRUNC, O_APPEND, access mode) */
#define O_RDONLY  0
#define O_WRONLY  1
#define O_RDWR    2
#define O_CREAT   0x40
#define O_TRUNC   0x200
#define O_APPEND  0x400

/* Allocate the lowest available fd in the running task. Returns -1 if full. */
static int fd_alloc(struct vfs_node *node, bool append)
{
    for (int i = 0; i < VFS_FD_MAX; i++) {
        if (!running_task->fds[i].open) {
            running_task->fds[i].open    = true;
            running_task->fds[i].append  = append;
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
    /* EBX = path, ECX = flags (Linux open(2) flags) */
    char resolved[VFS_PATH_MAX];
    const char *path = resolve_path((const char *)frame->ebx, resolved);
    int         flags = (int)frame->ecx;

    if (!path) return -1;

    int accmode = flags & 3; /* O_RDONLY=0, O_WRONLY=1, O_RDWR=2 */
    bool do_creat  = (flags & O_CREAT)  != 0;
    bool do_trunc  = (flags & O_TRUNC)  != 0;
    bool do_append = (flags & O_APPEND) != 0;

    struct vfs_node *node;

    if (do_creat || do_trunc) {
        /* Create or truncate: need write access */
        node = vfs_creat(path);
        if (!node) return -1;
    } else {
        node = vfs_lookup(path);
        if (!node) return -1;
        if (accmode != O_RDONLY && !(node->flags & VFS_CHARDEV) && !node->writable)
            return -1; /* read-only initrd file, write access denied */
    }

    return fd_alloc(node, do_append);
}

static int sys_close(struct trap_frame *frame)
{
    int fd = (int)frame->ebx;
    if (fd < 0 || fd >= VFS_FD_MAX) return -1;
    if (!running_task->fds[fd].open) return -1;
    struct vfs_node *node = running_task->fds[fd].node;
    running_task->fds[fd].open = false;
    running_task->fds[fd].node = NULL;
    if (node) pipe_notify_close(node);
    return 0;
}

static int sys_pipe(struct trap_frame *frame)
{
    /* EBX = pointer to int[2] in user space: [0]=read_fd, [1]=write_fd */
    int *fds = (int *)frame->ebx;
    if (!user_access_ok(fds, sizeof(int) * 2)) return -1;

    struct vfs_node *rnode, *wnode;
    if (pipe_create(&rnode, &wnode) < 0) return -1;

    int rfd = fd_alloc(rnode, false);
    if (rfd < 0) return -1;
    int wfd = fd_alloc(wnode, false);
    if (wfd < 0) {
        running_task->fds[rfd].open = false;
        running_task->fds[rfd].node = NULL;
        return -1;
    }

    fds[0] = rfd;
    fds[1] = wfd;
    return 0;
}

static int sys_dup2(struct trap_frame *frame)
{
    /* EBX = oldfd, ECX = newfd */
    int oldfd = (int)frame->ebx;
    int newfd = (int)frame->ecx;

    if (oldfd < 0 || oldfd >= VFS_FD_MAX) return -1;
    if (newfd < 0 || newfd >= VFS_FD_MAX) return -1;
    if (!running_task->fds[oldfd].open) return -1;
    if (oldfd == newfd) return newfd;

    /* Close newfd if open */
    if (running_task->fds[newfd].open) {
        struct vfs_node *old = running_task->fds[newfd].node;
        running_task->fds[newfd].open = false;
        running_task->fds[newfd].node = NULL;
        if (old) pipe_notify_close(old);
    }

    running_task->fds[newfd] = running_task->fds[oldfd];
    /* Bump pipe refcount for the new fd copy */
    if (running_task->fds[newfd].node)
        pipe_fork_fd(running_task->fds[newfd].node);
    return newfd;
}


static int sys_lseek(struct trap_frame *frame)
{
    int      fd     = (int)frame->ebx;
    uint32_t offset = frame->ecx;
    int      whence = (int)frame->edx;

    if (fd < 0 || fd >= VFS_FD_MAX) return -1;
    if (!running_task->fds[fd].open) return -1;

    struct vfs_fd *vfd = &running_task->fds[fd];
    if (!(vfd->node->flags & (VFS_FILE | VFS_CHARDEV))) return -1;

    /* Chardevs are not seekable — report position 0 as a no-op */
    if (vfd->node->flags & VFS_CHARDEV) return 0;

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
    char resolved[VFS_PATH_MAX];
    const char *path = resolve_path((const char *)frame->ebx, resolved);
    struct vfs_stat *out = (struct vfs_stat *)frame->ecx;

    if (!path) return -1;
    if (!user_access_ok(out, sizeof(struct vfs_stat))) return -1;

    return vfs_stat(path, out);
}

static int sys_mount(struct trap_frame *frame)
{
    (void)frame;
    /* Print VFS mount information to the kernel console */
    printk("mount:\n");
    printk("  / (initrd, cpio, rw — volatile)\n");
    return 0;
}

/* Resolve a user-supplied path against the running task's cwd.
 * Returns pointer to out_buf (VFS_PATH_MAX bytes) on success, NULL on error. */
static const char *resolve_path(const char *upath, char *out_buf)
{
    if (!user_access_ok(upath, 1)) return NULL;
    return vfs_resolve(running_task->cwd, upath, out_buf);
}

static int sys_chdir(struct trap_frame *frame)
{
    const char *path = (const char *)frame->ebx;
    char resolved[VFS_PATH_MAX];
    if (!resolve_path(path, resolved)) return -1;

    struct vfs_node *node = vfs_lookup(resolved);
    if (!node || !(node->flags & VFS_DIR)) return -1;

    uint32_t len = (uint32_t)strlen(resolved);
    if (len >= VFS_PATH_MAX) return -1;
    memcpy(running_task->cwd, resolved, len + 1);
    return 0;
}

static int sys_getcwd(struct trap_frame *frame)
{
    char *buf = (char *)frame->ebx;
    uint32_t len = frame->ecx;
    if (!user_access_ok(buf, len)) return -1;

    uint32_t cwdlen = (uint32_t)strlen(running_task->cwd) + 1;
    if (cwdlen > len) return -1;
    memcpy(buf, running_task->cwd, cwdlen);
    return 0;
}

static int sys_mkdir(struct trap_frame *frame)
{
    char resolved[VFS_PATH_MAX];
    const char *path = resolve_path((const char *)frame->ebx, resolved);
    if (!path) return -1;
    return vfs_mkdir(path) ? 0 : -1;
}

static int sys_unlink(struct trap_frame *frame)
{
    char resolved[VFS_PATH_MAX];
    const char *path = resolve_path((const char *)frame->ebx, resolved);
    if (!path) return -1;
    return vfs_unlink(path);
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
    case SYS_READ:
        ret = sys_read(frame);
        break;
    case SYS_READ_NB:
        ret = sys_read_nb(frame);
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
        ret = sys_ps(frame);
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
    case SYS_KILL:
        ret = sys_kill(frame);
        break;
    case SYS_YIELD:
        ret = sys_yield();
        break;
    case SYS_WIN_GET_SIZE:
        ret = sys_win_get_size(frame);
        break;
    case SYS_OPEN:
        ret = sys_open(frame);
        break;
    case SYS_CLOSE:
        ret = sys_close(frame);
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
    case SYS_PIPE:
        ret = sys_pipe(frame);
        break;
    case SYS_DUP2:
        ret = sys_dup2(frame);
        break;
    case SYS_TASK_DONE:
        ret = sys_task_done(frame);
        break;
    case SYS_CHDIR:
        ret = sys_chdir(frame);
        break;
    case SYS_GETCWD:
        ret = sys_getcwd(frame);
        break;
    case SYS_GETPPID:
        ret = sys_getppid();
        break;
    case SYS_MKDIR:
        ret = sys_mkdir(frame);
        break;
    case SYS_UNLINK:
        ret = sys_unlink(frame);
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
