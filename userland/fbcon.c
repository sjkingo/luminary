/* fbcon.c — framebuffer console daemon.
 *
 * Replaces the in-kernel interactive console.  fbcon:
 *   1. Gets framebuffer geometry from /dev/fb0.
 *   2. Opens /dev/console to read kernel stdout output.
 *   3. Forks /bin/sh, connecting its stdout/stderr to an internal pipe that
 *      fbcon reads and renders, and its stdin to a relay from fbcon's own stdin.
 *   4. Runs an event loop:
 *        - Read keyboard from stdin (fd 0), relay bytes to shell stdin pipe.
 *          Page Up/Down sentinels (0x01/0x02) are handled locally for scrollback.
 *          All other sentinel bytes (0x10-0x16) were discarded by the kernel's
 *          stdin_read_op; they do not reach fbcon's stdin.
 *        - Read shell output (non-blocking) -> termemu_putchar -> render dirty rows.
 *        - Read kernel console pipe (/dev/console) (non-blocking) -> same render path.
 *        - On shell exit, respawn.
 *   5. Suppresses framebuffer rendering while the GUI compositor has the keyboard,
 *      to avoid stomping on GUI output and to reduce CPU overhead.
 *
 * Rendering is via the shared termemu library + fbrender.h inline helpers.
 */

#include "syscall.h"
#include "libc/stdlib.h"
#include "libc/termemu.h"
#include "fb_dev.h"
#include "sys_dev.h"

#include "libc/fbrender.h"  /* must come after termemu.h (uses struct termemu) */

#define FONT_W  CONSOLE_FONT_WIDTH
#define FONT_H  CONSOLE_FONT_HEIGHT

static struct termemu temu;
static struct fb_info fbi;
static int sysfd = -1;   /* /dev/sys fd for GUI-active queries */


/* Returns 1 if the GUI compositor currently owns the keyboard. */
static int gui_active(void)
{
    unsigned int v = 0;
    if (sysfd < 0) return 0;
    if (ioctl(sysfd, SYS_CTL_GUI_ACTIVE, &v) < 0) return 0;
    return (int)v;
}

static int render_dirty(void)
{
    int r;
    int first = -1;
    int last  = -1;
    if (gui_active()) return 0;
    for (r = 0; r < temu.rows; r++) {
        if (!termemu_is_dirty(&temu, r))
            continue;
        fbrender_render_row(&temu, r);
        termemu_clear_dirty(&temu, r);
        if (first < 0) first = r;
        last = r;
    }
    if (first >= 0) {
        fbrender_render_cursor(&temu);
        /* Cursor row may be outside the dirty range — extend blit to cover it */
        if (temu.scroll_offset == 0) {
            if (temu.cur_row < first) first = temu.cur_row;
            if (temu.cur_row > last)  last  = temu.cur_row;
        }
        fbrender_present_rows(first, last);
    }
    return first >= 0;
}

static void render_all(void)
{
    termemu_mark_all_dirty(&temu);
    render_dirty();
}

static void run_session(int console_fd)
{
    int shell_in[2];
    int shell_out[2];
    int child;
    int i;
    int n;
    char buf[256];
    char kbd[64];

    if (pipe(shell_in) < 0 || pipe(shell_out) < 0)
        return;

    child = fork();
    if (child < 0) {
        vfs_close(shell_in[0]); vfs_close(shell_in[1]);
        vfs_close(shell_out[0]); vfs_close(shell_out[1]);
        return;
    }

    if (child == 0) {
        dup2(shell_in[0],  0);
        dup2(shell_out[1], 1);
        dup2(shell_out[1], 2);
        vfs_close(shell_in[0]);
        vfs_close(shell_in[1]);
        vfs_close(shell_out[0]);
        vfs_close(shell_out[1]);
        if (console_fd >= 0) vfs_close(console_fd);
        if (sysfd >= 0) vfs_close(sysfd);

        char *sh_argv[] = { "/bin/sh", (char *)0 };
        execv("/bin/sh", sh_argv);
        exit(1);
    }

    vfs_close(shell_in[0]);
    vfs_close(shell_out[1]);

    /* Drain any buffered kernel output (boot messages) before the shell starts */
    if (console_fd >= 0) {
        n = read_nb(console_fd, buf, sizeof(buf) - 1);
        while (n > 0) {
            for (i = 0; i < n; i++)
                termemu_putchar(&temu, buf[i]);
            n = read_nb(console_fd, buf, sizeof(buf) - 1);
        }
        render_all();
    }

    int prev_gui = 0;

    for (;;) {
        int got_output = 0;
        int cur_gui = gui_active();

        /* When GUI just released the keyboard, repaint the console */
        if (prev_gui && !cur_gui)
            render_all();
        prev_gui = cur_gui;

        /* Drain all available output before rendering to avoid visible
         * line-by-line updates when a command produces multi-chunk output. */
        if (console_fd >= 0) {
            n = read_nb(console_fd, buf, sizeof(buf) - 1);
            while (n > 0) {
                for (i = 0; i < n; i++)
                    termemu_putchar(&temu, buf[i]);
                got_output = 1;
                n = read_nb(console_fd, buf, sizeof(buf) - 1);
            }
        }

        n = read_nb(shell_out[0], buf, sizeof(buf) - 1);
        while (n > 0) {
            for (i = 0; i < n; i++)
                termemu_putchar(&temu, buf[i]);
            got_output = 1;
            n = read_nb(shell_out[0], buf, sizeof(buf) - 1);
        }

        if (got_output)
            render_dirty();

        /* Read keyboard from our stdin and relay to shell */
        n = read_nb(0, kbd, sizeof(kbd));
        if (n > 0) {
            int out = 0;
            for (i = 0; i < n; i++) {
                unsigned char c = (unsigned char)kbd[i];
                if (c == 0x01) {         /* KEY_PGUP — scroll back */
                    termemu_scroll_up(&temu);
                    render_all();
                } else if (c == 0x02) {  /* KEY_PGDN — scroll forward */
                    termemu_scroll_down(&temu);
                    render_all();
                } else {
                    kbd[out++] = kbd[i];
                }
            }
            if (out > 0)
                write(shell_in[1], kbd, (unsigned int)out);
        }

        if (task_done(child))
            break;

        yield();
    }

    vfs_close(shell_in[1]);
    vfs_close(shell_out[0]);
}

int main(int argc, char **argv)
{
    int fbfd;
    int cols;
    int rows;
    int console_fd;
    const char *p;

    (void)argc; (void)argv;

    fbfd = open("/dev/fb0", O_RDWR);
    if (fbfd < 0 || fb_get_info(fbfd, &fbi) < 0)
        return 1;
    vfs_close(fbfd);

    sysfd = open("/dev/sys", O_RDWR);

    cols = (int)(fbi.width  / (unsigned int)FONT_W);
    rows = (int)(fbi.height / (unsigned int)FONT_H);

    if (cols > TERMEMU_MAX_COLS) cols = TERMEMU_MAX_COLS;
    if (rows > TERMEMU_MAX_ROWS) rows = TERMEMU_MAX_ROWS;

    fbrender_init((unsigned char *)(unsigned int)fbi.fb_addr,
                  fbi.width, fbi.height, fbi.depth, fbi.pitch);

    {
        unsigned int shadow_size = fbi.pitch * fbi.height;
        unsigned char *shadow = (unsigned char *)malloc(shadow_size);
        if (shadow)
            fbrender_set_shadow(shadow);
    }

    termemu_init(&temu, cols, rows, TERMEMU_MAX_SB);

    /* Open /dev/console once and keep it open for the lifetime of fbcon.
     * Closing it would drop read_refs to 0, freeing the pipe and leaving
     * dev.c's console_pipe as a dangling pointer. */
    console_fd = open("/dev/console", O_RDONLY);

    /* Clear the screen (overwrites the dumb kernel fbdev output).
     * run_session will drain any buffered boot messages before rendering. */
    fbrender_clear_rect(0, 0, fbi.width, fbi.height);
    fbrender_present();

    for (;;) {
        run_session(console_fd);
        /* Shell exited — show respawn message and loop */
        termemu_putchar(&temu, '\n');
        for (p = "[fbcon: shell exited, respawning]\n"; *p; p++)
            termemu_putchar(&temu, *p);
        render_dirty();
    }
}
