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
#include "kernel/vmm.h"
#include "kernel/pmm.h"
#include "kernel/vfs.h"
#include "boot/multiboot.h"
#include "cpu/traps.h"
#include "cpu/x86.h"
#include "drivers/keyboard.h"
#include "drivers/blkdev.h"

/* Linux open(2) flag bits */
#define O_RDONLY   0
#define O_WRONLY   1
#define O_RDWR     2
#define O_CREAT    0x40
#define O_TRUNC    0x200
#define O_APPEND   0x400
#define O_NONBLOCK 0x800

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

    if ((vfd->node->flags & VFS_CHARDEV) && !(vfd->node->flags & VFS_FILE)) {
        vfs_write(vfd->node, 0, len, buf);
        return (int)len;
    }

    if (vfd->node->flags & (VFS_FILE | VFS_CHARDEV)) {
        uint32_t off = vfd->append ? vfd->node->size : vfd->offset;
        uint32_t n = vfs_write(vfd->node, off, len, buf);
        vfd->offset = off + n;
        return (int)n;
    }

    return -1;
}


static int sys_ioctl(struct trap_frame *frame)
{
    /* EBX=fd, ECX=request, EDX=arg (user pointer or scalar) */
    int fd          = (int)frame->ebx;
    uint32_t req    = frame->ecx;
    void *arg       = (void *)frame->edx;

    if (fd < 0 || fd >= VFS_FD_MAX) return -1;
    struct vfs_fd *vfd = &running_task->fds[fd];
    if (!vfd->open || !vfd->node) return -1;

    /* If arg is non-NULL it must point into user space. */
    if (arg && !user_access_ok(arg, 1)) return -1;

    return (int)vfs_ioctl(vfd->node, req, arg);
}


static int sys_yield(void)
{
    enable_interrupts();
    if (running_task) running_task->blocking = true;
    asm volatile("hlt");
    if (running_task) running_task->blocking = false;
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

    struct vfs_fd *vfd = &running_task->fds[fd];
    if (!vfd->open || !vfd->node) return -1;

    /* Pure chardevs (devices, pipes): no offset tracking, cap at 4096 */
    if ((vfd->node->flags & VFS_CHARDEV) && !(vfd->node->flags & VFS_FILE)) {
        if (!user_access_ok(buf, len) || len > 4096) return -1;
        if (vfd->nonblock) running_task->read_nonblock = true;
        uint32_t n = vfs_read(vfd->node, 0, len, buf);
        running_task->read_nonblock = false;
        return (int)n;
    }

    /* Regular files and file+chardev nodes (e.g. ext2): offset-tracked */
    if (vfd->node->flags & (VFS_FILE | VFS_CHARDEV)) {
        if (!user_access_ok(buf, len) || len > 65536) return -1;
        if (vfd->nonblock) running_task->read_nonblock = true;
        uint32_t n = vfs_read(vfd->node, vfd->offset, len, buf);
        running_task->read_nonblock = false;
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

static int sys_getpid(void)
{
    return (int)running_task->pid;
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

/* Parse a shebang line "#!interpreter [arg]" from the first line of data.
 * Writes NUL-terminated interpreter path into interp_buf (size interp_sz)
 * and optional argument into arg_buf (size arg_sz); arg_buf[0]=='\0' if none.
 * Returns 1 if a shebang was found, 0 otherwise. */
static int parse_shebang(const char *data, uint32_t size,
                          char *interp_buf, uint32_t interp_sz,
                          char *arg_buf,    uint32_t arg_sz)
{
    if (size < 2 || data[0] != '#' || data[1] != '!')
        return 0;

    const char *p = data + 2;
    const char *end = data + size;

    /* skip optional spaces after #! */
    while (p < end && (*p == ' ' || *p == '\t')) p++;

    /* read interpreter path */
    uint32_t n = 0;
    while (p < end && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r' && n < interp_sz - 1)
        interp_buf[n++] = *p++;
    interp_buf[n] = '\0';
    if (n == 0) return 0;

    /* skip spaces between interpreter and optional arg */
    while (p < end && (*p == ' ' || *p == '\t')) p++;

    /* read optional arg */
    n = 0;
    while (p < end && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r' && n < arg_sz - 1)
        arg_buf[n++] = *p++;
    arg_buf[n] = '\0';

    return 1;
}

static int sys_execve(struct trap_frame *frame)
{
    /* EBX=path, ECX=argv[], EDX=envp[] (NULL = inherit existing environ) */
    char resolved[VFS_PATH_MAX];
    const char *path = resolve_path((const char *)frame->ebx, resolved);
    uint32_t argv_ptr = frame->ecx;
    uint32_t envp_ptr = frame->edx;

    if (!path) return -1;

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

    /* Copy envp strings to kernel scratch before the address space is replaced */
    static char env_scratch[TASK_ENVIRON_MAX][TASK_ENVIRON_LEN];
    int envc = 0;
    if (envp_ptr && user_access_ok((void *)envp_ptr, 4)) {
        uint32_t *uenvp = (uint32_t *)envp_ptr;
        while (envc < TASK_ENVIRON_MAX && uenvp[envc] &&
               user_access_ok((void *)uenvp[envc], 1)) {
            strncpy(env_scratch[envc], (const char *)uenvp[envc], TASK_ENVIRON_LEN - 1);
            env_scratch[envc][TASK_ENVIRON_LEN - 1] = '\0';
            envc++;
        }
    }

    struct vfs_node *exec_node = vfs_lookup(path);
    if (!exec_node || !(exec_node->flags & VFS_FILE)) {
        printk("execve: '%s' not found\n", path);
        return -1;
    }
    uint32_t elf_size = exec_node->size;
    void *elf_data = kmalloc(elf_size);
    if (!elf_data) return -1;
    if (vfs_read(exec_node, 0, elf_size, elf_data) != elf_size) {
        kfree(elf_data);
        printk("execve: '%s' short read\n", path);
        return -1;
    }

    static char shebang_interp[VFS_PATH_MAX];
    static char shebang_arg[128];
    static char shebang_script[VFS_PATH_MAX];
    if (parse_shebang((const char *)elf_data, elf_size,
                      shebang_interp, sizeof(shebang_interp),
                      shebang_arg,    sizeof(shebang_arg))) {
        kfree(elf_data);

        char interp_resolved[VFS_PATH_MAX];
        if (!vfs_resolve("/", shebang_interp, interp_resolved)) {
            printk("execve: shebang interpreter '%s' not found\n", shebang_interp);
            return -1;
        }
        struct vfs_node *interp_node = vfs_lookup(interp_resolved);
        if (!interp_node || !(interp_node->flags & VFS_FILE)) {
            printk("execve: shebang interpreter '%s' not found\n", interp_resolved);
            return -1;
        }
        uint32_t interp_size = interp_node->size;
        void *interp_data = kmalloc(interp_size);
        if (!interp_data) return -1;
        if (vfs_read(interp_node, 0, interp_size, interp_data) != interp_size) {
            kfree(interp_data);
            return -1;
        }

        static const char *sargv[32];
        int sargc = 0;
        sargv[sargc++] = shebang_interp;
        if (shebang_arg[0])
            sargv[sargc++] = shebang_arg;
        memcpy(shebang_script, resolved, strlen(resolved) + 1);
        sargv[sargc++] = shebang_script;
        for (int i = 1; i < argc && sargc < 31; i++)
            sargv[sargc++] = kargv[i];
        sargv[sargc] = NULL;

        int r = task_exec(interp_data, interp_size, frame, sargc, sargv);
        kfree(interp_data);
        if (r == 0) {
            running_task->environ_count = 0;
            for (int i = 0; i < envc; i++) {
                strncpy(running_task->environ[i], env_scratch[i], TASK_ENVIRON_LEN - 1);
                running_task->environ[i][TASK_ENVIRON_LEN - 1] = '\0';
            }
            running_task->environ_count = envc;
            cpu_reset_fault_counter();
        }
        return r;
    }

    int r = task_exec(elf_data, elf_size, frame, argc, kargv);
    kfree(elf_data);
    if (r == 0) {
        running_task->environ_count = 0;
        for (int i = 0; i < envc; i++) {
            strncpy(running_task->environ[i], env_scratch[i], TASK_ENVIRON_LEN - 1);
            running_task->environ[i][TASK_ENVIRON_LEN - 1] = '\0';
        }
        running_task->environ_count = envc;
        cpu_reset_fault_counter();
    }
    return r;
}

static int sys_fork(struct trap_frame *frame)
{
    /* Bump pipe refcounts for the child's inherited fds BEFORE forking.
     * task_fork re-enables interrupts after inserting the child into the
     * scheduler queue, creating a window where the child runs and closes
     * pipe fds before the parent can increment refcounts.  Doing the
     * increments first (on the current task's fd table, which is identical
     * to what the child will inherit) eliminates the race entirely. */
    for (int i = 0; i < VFS_FD_MAX; i++) {
        if (running_task->fds[i].open && running_task->fds[i].node)
            pipe_fork_fd(running_task->fds[i].node);
    }

    struct task *child = task_fork(frame);
    if (!child) {
        /* Fork failed — undo the refcount increments */
        for (int i = 0; i < VFS_FD_MAX; i++) {
            if (running_task->fds[i].open && running_task->fds[i].node)
                pipe_notify_close(running_task->fds[i].node);
        }
        return -1;
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

    /* WNOHANG: return 0 (child still running) if it hasn't exited yet */
    if (flags & WNOHANG) return 0;

    /* Block until the child exits */
    running_task->wait_pid  = target_pid;
    running_task->wait_done = false;
    running_task->prio_d    = SCHED_LEVEL_SUSP;

    while (!running_task->wait_done) {
        enable_interrupts();
        running_task->blocking = true;
        asm volatile("hlt");
        running_task->blocking = false;
        disable_interrupts();
    }

    running_task->wait_done = false;
    if (status) *status = running_task->exit_status;
    DBGK("waitpid: pid %d unblocked, returning %d\n", running_task->pid, target_pid);
    return target_pid;
}

static int sys_task_done(struct trap_frame *frame)
{
    /* Returns 1 if pid is no longer in the scheduler queue, 0 if still running */
    unsigned int target_pid = frame->ebx;
    struct task *t = sched_queue;
    while (t) {
        if (t->pid == target_pid)
            return 0;
        t = t->next;
    }
    DBGK("task_done: pid=%ld not found (dead), caller pid=%ld\n",
         (long)target_pid,
         running_task ? (long)running_task->pid : -1L);
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

    if (do_trunc) {
        /* Truncate (always creates/resets the file) */
        node = vfs_creat(path);
        if (!node) return -1;
    } else if (do_creat) {
        /* Create only if missing; do not truncate existing file */
        node = vfs_lookup(path);
        if (!node) {
            node = vfs_creat(path);
            if (!node) return -1;
        }
        if (accmode != O_RDONLY && !(node->flags & VFS_CHARDEV) && !node->writable)
            return -1;
    } else {
        node = vfs_lookup(path);
        if (!node) return -1;
        if (accmode != O_RDONLY && !(node->flags & VFS_CHARDEV) && !node->writable)
            return -1;
    }

    int fd = fd_alloc(node, do_append);
    if (fd < 0) return -1;
    if (do_append)
        running_task->fds[fd].offset = node->size;
    pipe_fork_fd(node);
    return fd;
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

static int sys_fstat(struct trap_frame *frame)
{
    int fd = (int)frame->ebx;
    struct vfs_stat *out = (struct vfs_stat *)frame->ecx;

    if (fd < 0 || fd >= VFS_FD_MAX) return -1;
    if (!user_access_ok(out, sizeof(struct vfs_stat))) return -1;

    struct vfs_fd *vfd = &running_task->fds[fd];
    if (!vfd->open || !vfd->node) return -1;

    return vfs_fstat(vfd->node, out);
}

static int sys_rename(struct trap_frame *frame)
{
    char old_resolved[VFS_PATH_MAX];
    char new_resolved[VFS_PATH_MAX];
    const char *old_path = resolve_path((const char *)frame->ebx, old_resolved);
    const char *new_path = resolve_path((const char *)frame->ecx, new_resolved);

    if (!old_path || !new_path) return -1;
    return vfs_rename(old_path, new_path);
}

static int sys_mount(struct trap_frame *frame)
{
    /* EBX = fstype string, ECX = mountpoint path, EDX = device path (or 0) */
    const char *fstype = (const char *)frame->ebx;
    const char *path   = (const char *)frame->ecx;
    const char *devarg = (const char *)frame->edx;

    if (!user_access_ok(fstype, 1)) return -1;
    char resolved[VFS_PATH_MAX];
    if (!resolve_path(path, resolved)) return -1;

    void *device = NULL;
    if (devarg && user_access_ok(devarg, 1)) {
        const char *devname = devarg;
        if (devname[0]=='/' && devname[1]=='d' && devname[2]=='e' &&
            devname[3]=='v' && devname[4]=='/') devname += 5;
        device = blkdev_find(devname);
        if (!device) {
            printk("mount: device '%s' not found\n", devarg);
            return -1;
        }
    }

    return vfs_do_mount(resolved, fstype, device);
}

static int sys_umount(struct trap_frame *frame)
{
    /* EBX = mountpoint path */
    char resolved[VFS_PATH_MAX];
    if (!resolve_path((const char *)frame->ebx, resolved)) return -1;
    return vfs_do_umount(resolved);
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
    case SYS_GETPID:
        ret = sys_getpid();
        break;
    case SYS_KILL:
        ret = sys_kill(frame);
        break;
    case SYS_YIELD:
        ret = sys_yield();
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
    case SYS_EXECVE:
        ret = sys_execve(frame);
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
    case SYS_IOCTL:
        ret = sys_ioctl(frame);
        break;

    case SYS_MOUNT:
        ret = sys_mount(frame);
        break;
    case SYS_UMOUNT:
        ret = sys_umount(frame);
        break;
    case SYS_FSTAT:
        ret = sys_fstat(frame);
        break;
    case SYS_RENAME:
        ret = sys_rename(frame);
        break;
    case SYS_BRK: {
        uint32_t new_brk = (uint32_t)frame->ebx;
        uint32_t cur_brk = running_task->brk;
        if (new_brk == 0 || new_brk <= cur_brk) {
            ret = (int)cur_brk;
        } else if (new_brk >= USER_STACK_TOP - PAGE_SIZE) {
            ret = (int)cur_brk;  /* refuse — too close to stack */
        } else {
            for (uint32_t v = cur_brk; v < new_brk; v += PAGE_SIZE) {
                uint32_t f = pmm_alloc_frame();
                if (!f) { new_brk = v; break; }
                vmm_map_page_in(running_task->page_dir_phys, v, f,
                                PTE_PRESENT | PTE_WRITE | PTE_USER);
            }
            running_task->brk = new_brk;
            ret = (int)new_brk;
        }
        break;
    }
    default:
        printk("unknown syscall %d from pid %d\n",
               frame->eax, running_task->pid);
        ret = -1;
        break;
    }

    /* Return value goes in EAX */
    frame->eax = (unsigned int)ret;
}
