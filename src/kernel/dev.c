/* dev.c — character device nodes for /dev/stdin, /dev/stdout, /dev/stderr.
 *
 * init_devfs() creates a /dev directory in the VFS tree and populates it
 * with three chardev nodes backed by the keyboard and framebuffer console.
 * After init_devfs() runs, task_open_std_fds() pre-opens fds 0/1/2 on
 * every new task so that sys_read/sys_write can dispatch through the VFS.
 */

#include <stdint.h>
#include <string.h>

#include "kernel/dev.h"
#include "kernel/kernel.h"
#include "kernel/sched.h"
#include "kernel/vfs.h"
#include "kernel/task.h"
#include "cpu/x86.h"
#include "drivers/keyboard.h"
#include "drivers/fbdev.h"

/* ── exported device node pointers ──────────────────────────────────────── */
struct vfs_node *dev_stdin  = NULL;
struct vfs_node *dev_stdout = NULL;
struct vfs_node *dev_stderr = NULL;

/* ── chardev ops ─────────────────────────────────────────────────────────── */

/* Blocking keyboard read — exact semantics as the old sys_read inner loop.
 * Yields while the keyboard is owned by the GUI compositor.
 * Filters Page Up/Down sentinels for in-kernel scrollback. */
static uint32_t stdin_read_op(uint32_t offset, uint32_t len, void *buf)
{
    (void)offset;
    char *cbuf = (char *)buf;

    for (;;) {
        /* While the GUI compositor owns the keyboard, block without consuming
         * any input so keystrokes route to the compositor. */
        if (kbd_is_owned()) {
            enable_interrupts();
            asm volatile("hlt");
            disable_interrupts();
            continue;
        }

        int n = keyboard_read(cbuf, len);

        /* Filter scroll sentinels and handle them in-kernel */
        int out = 0;
        for (int i = 0; i < n; i++) {
            if (cbuf[i] == KEY_PGUP)
                fbdev_scroll_up();
            else if (cbuf[i] == KEY_PGDN)
                fbdev_scroll_down();
            else
                cbuf[out++] = cbuf[i];
        }

        if (out > 0)
            return (uint32_t)out;

        /* Non-blocking mode: return immediately with 0 bytes */
        if (running_task && running_task->read_nonblock)
            return 0;

        enable_interrupts();
        asm volatile("hlt");
        disable_interrupts();
    }
}

static uint32_t stdout_write_op(uint32_t offset, uint32_t len, const void *buf)
{
    (void)offset;
    const char *cbuf = (const char *)buf;
    for (uint32_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)cbuf[i];
        if (c == '\n' || c == '\r' || c == '\t' || c == '\b' ||
            (c >= 0x20 && c <= 0x7E)) {
            printk("%c", c);
        } else {
            /* non-printable: render as \xNN */
            static const char hex[] = "0123456789abcdef";
            char esc[4] = { '\\', 'x', hex[c >> 4], hex[c & 0xF] };
            for (int j = 0; j < 4; j++)
                printk("%c", esc[j]);
        }
    }
    return len;
}

/* ── init_devfs ──────────────────────────────────────────────────────────── */

void init_devfs(void)
{
    struct vfs_node *root = vfs_get_root();
    if (!root)
        panic("init_devfs: VFS root not mounted");

    /* Create /dev directory */
    struct vfs_node *dev_dir = vfs_alloc_node();
    if (!dev_dir)
        panic("init_devfs: node pool exhausted");
    strncpy(dev_dir->name, "dev", VFS_NAME_MAX - 1);
    dev_dir->flags = VFS_DIR;
    dev_dir->inode = 99;
    vfs_add_child(root, dev_dir);

    /* /dev/stdin — readable chardev backed by keyboard */
    dev_stdin = vfs_alloc_node();
    if (!dev_stdin) panic("init_devfs: node pool exhausted");
    strncpy(dev_stdin->name, "stdin", VFS_NAME_MAX - 1);
    dev_stdin->flags    = VFS_FILE | VFS_CHARDEV;
    dev_stdin->inode    = 100;
    dev_stdin->read_op  = stdin_read_op;
    dev_stdin->write_op = NULL;
    vfs_add_child(dev_dir, dev_stdin);

    /* /dev/stdout — writable chardev backed by framebuffer console */
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

    printk("devfs: /dev/stdin, /dev/stdout, /dev/stderr ready\n");
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
