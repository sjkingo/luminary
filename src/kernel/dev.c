/* dev.c — character device nodes for /dev/stdin, /dev/stdout, /dev/stderr,
 *          and /dev/console.
 *
 * init_devfs() creates a /dev directory in the VFS tree, registers the
 * standard stream chardevs, and creates the console pipe.
 *
 * Console pipe: stdout_write_op writes to a kernel-managed pipe (non-blocking,
 * drops bytes if full — never stalls the kernel).  fbcon opens /dev/console
 * to read from the pipe read end and renders output to the framebuffer.
 * writechar_fb is kept as a fallback for the early-boot window before fbcon
 * starts consuming the pipe.
 *
 * After init_devfs() runs, task_open_std_fds() pre-opens fds 0/1/2 on
 * every new task so that sys_read/sys_write can dispatch through the VFS.
 */

#include <stdint.h>
#include <string.h>

#include "kernel/dev.h"
#include "kernel/kernel.h"
#include "kernel/pipe.h"
#include "kernel/sched.h"
#include "kernel/vfs.h"
#include "kernel/task.h"
#include "cpu/x86.h"
#include "drivers/keyboard.h"
#include "drivers/fbdev.h"
#include "drivers/serial.h"

/* ── exported device node pointers ──────────────────────────────────────── */
struct vfs_node *dev_stdin  = NULL;
struct vfs_node *dev_stdout = NULL;
struct vfs_node *dev_stderr = NULL;

/* ── console pipe ────────────────────────────────────────────────────────── */
/* console_pipe: write end used by stdout_write_op; read end exposed as
 * /dev/console for fbcon to read.  NULL until init_devfs creates it. */
static struct pipe *console_pipe = NULL;

/* ── chardev ops ─────────────────────────────────────────────────────────── */

/* Blocking keyboard read.
 * Yields while the keyboard is owned by the GUI compositor.
 * Discards navigation sentinel bytes (KEY_UP..KEY_DEL) — fbcon handles
 * these via its own stdin relay; the raw shell on the fbdev console has
 * no cursor movement and should never receive them. */
static uint32_t stdin_read_op(uint32_t offset, uint32_t len, void *buf)
{
    (void)offset;
    char *cbuf = (char *)buf;

    for (;;) {
        /* While the GUI compositor owns the keyboard, block without consuming
         * any input so keystrokes route to the compositor.
         * Non-blocking callers (read_nb) must not block here — return 0. */
        if (kbd_is_owned()) {
            if (running_task && running_task->read_nonblock)
                return 0;
            enable_interrupts();
            if (running_task) running_task->blocking = true;
            asm volatile("hlt");
            if (running_task) running_task->blocking = false;
            disable_interrupts();
            continue;
        }

        int n = keyboard_read(cbuf, len);

        int out = 0;
        for (int i = 0; i < n; i++) {
            unsigned char c = (unsigned char)cbuf[i];
            /* Discard navigation sentinels — fbcon handles these via its own
             * stdin relay loop; they have no meaning on the raw fbdev console */
            if (c == KEY_UP    || c == KEY_DOWN  || c == KEY_LEFT   ||
                c == KEY_RIGHT  || c == KEY_HOME  || c == KEY_END    ||
                c == KEY_DEL    || c == KEY_ALT_F4)
                continue;
            cbuf[out++] = cbuf[i];
        }

        if (out > 0) {
            DBGK("stdin_read_op: pid=%ld returning %d bytes nb=%d\n",
                 running_task ? (long)running_task->pid : 0L,
                 out, running_task && running_task->read_nonblock ? 1 : 0);
            return (uint32_t)out;
        }

        /* Non-blocking mode: return immediately with 0 bytes */
        if (running_task && running_task->read_nonblock)
            return 0;

        enable_interrupts();
        if (running_task) running_task->blocking = true;
        asm volatile("hlt");
        if (running_task) running_task->blocking = false;
        disable_interrupts();
    }
}

static uint32_t stdout_write_op(uint32_t offset, uint32_t len, const void *buf)
{
    (void)offset;
    static const char hex[] = "0123456789abcdef";
    const unsigned char *cbuf = (const unsigned char *)buf;

    for (uint32_t i = 0; i < len; i++) {
        unsigned char c = cbuf[i];

        if (c == '\n' || c == '\r' || c == '\t' || c == '\b' ||
            (c >= 0x20 && c <= 0x7E)) {
            if (console_pipe)
                pipe_write_nb(console_pipe, (const char *)&c, 1);
            else
                writechar_fb((char)c);
            write_serial((char)c);
        } else {
            /* non-printable: render as \xNN */
            char esc[4] = { '\\', 'x', hex[c >> 4], hex[c & 0xF] };
            if (console_pipe)
                pipe_write_nb(console_pipe, esc, 4);
            else {
                for (int j = 0; j < 4; j++)
                    writechar_fb(esc[j]);
            }
            for (int j = 0; j < 4; j++)
                write_serial(esc[j]);
        }
    }
    return len;
}

/* ── init_devfs ──────────────────────────────────────────────────────────── */

void init_devfs(void)
{
    struct vfs_node *read_node;
    struct vfs_node *write_node;
    struct vfs_node *dev_dir;
    struct vfs_node *root = vfs_get_root();
    if (!root)
        panic("init_devfs: VFS root not mounted");

    /* Create /dev directory */
    dev_dir = vfs_alloc_node();
    if (!dev_dir)
        panic("init_devfs: node pool exhausted");
    strncpy(dev_dir->name, "dev", VFS_NAME_MAX - 1);
    dev_dir->flags = VFS_DIR;
    dev_dir->inode = 99;
    vfs_add_child(root, dev_dir);
    vfs_mount("/dev", "devfs", dev_dir);

    /* /dev/stdin — readable chardev backed by keyboard */
    dev_stdin = vfs_alloc_node();
    if (!dev_stdin) panic("init_devfs: node pool exhausted");
    strncpy(dev_stdin->name, "stdin", VFS_NAME_MAX - 1);
    dev_stdin->flags    = VFS_FILE | VFS_CHARDEV;
    dev_stdin->inode    = 100;
    dev_stdin->read_op  = stdin_read_op;
    dev_stdin->write_op = NULL;
    vfs_add_child(dev_dir, dev_stdin);

    /* /dev/stdout — writable chardev routed through the console pipe */
    dev_stdout = vfs_alloc_node();
    if (!dev_stdout) panic("init_devfs: node pool exhausted");
    strncpy(dev_stdout->name, "stdout", VFS_NAME_MAX - 1);
    dev_stdout->flags    = VFS_FILE | VFS_CHARDEV;
    dev_stdout->inode    = 101;
    dev_stdout->read_op  = NULL;
    dev_stdout->write_op = stdout_write_op;
    vfs_add_child(dev_dir, dev_stdout);

    /* /dev/stderr — same write op as stdout */
    dev_stderr = vfs_alloc_node();
    if (!dev_stderr) panic("init_devfs: node pool exhausted");
    strncpy(dev_stderr->name, "stderr", VFS_NAME_MAX - 1);
    dev_stderr->flags    = VFS_FILE | VFS_CHARDEV;
    dev_stderr->inode    = 102;
    dev_stderr->read_op  = NULL;
    dev_stderr->write_op = stdout_write_op;
    vfs_add_child(dev_dir, dev_stderr);

    /* Create the console pipe.  stdout_write_op writes to the write end;
     * /dev/console exposes the read end for fbcon to drain.
     * The write_node is kept internal (kernel-only); read_node is published
     * under /dev/console so fbcon can open it with full pipe semantics. */
    if (pipe_create(&read_node, &write_node) == 0) {
        console_pipe = pipe_get_struct(write_node);
        strncpy(read_node->name, "console", VFS_NAME_MAX - 1);
        read_node->inode = 103;
        vfs_add_child(dev_dir, read_node);
    }

    printk("devfs: devfs mounted at /dev\n");
}

/* ── task_open_std_fds ───────────────────────────────────────────────────── */

void task_open_std_fds(struct task *t)
{
    /* Guard: init_devfs may not have run yet (idle task, early kernel tasks) */
    if (!dev_stdin || !dev_stdout || !dev_stderr)
        return;

    t->fds[0].open    = true;
    t->fds[0].node    = dev_stdin;
    t->fds[0].offset  = 0;
    t->fds[0].dir_idx = 0;

    t->fds[1].open    = true;
    t->fds[1].node    = dev_stdout;
    t->fds[1].offset  = 0;
    t->fds[1].dir_idx = 0;

    t->fds[2].open    = true;
    t->fds[2].node    = dev_stderr;
    t->fds[2].offset  = 0;
    t->fds[2].dir_idx = 0;
}
