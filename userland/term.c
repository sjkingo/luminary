/* term.c — terminal emulator window running /bin/sh
 *
 * Creates a GUI window, forks /bin/sh with its stdin/stdout connected via
 * pipes, then runs an event loop:
 *   - GUI keypress events  → translate sentinels to ANSI, write to shell stdin pipe
 *   - shell stdout pipe    → termemu_putchar → render dirty rows to window
 *
 * Font: 8×16 (matches kernel fbdev).  Default size: 80×24 cells.
 * Scrollback: 512 lines, Page Up/Down to navigate.
 */

#include "syscall.h"
#include "x.h"
#include "libc/termemu.h"

static int xfd = -1;

#define TERM_COLS   80
#define TERM_ROWS   24
#define FONT_W       8
#define FONT_H      16
#define CLIENT_W    (TERM_COLS * FONT_W)
#define CLIENT_H    (TERM_ROWS * FONT_H)

/* Solarized Dark colours */
#define COL_BG      rgb(  0,  26,  33)
#define COL_FG      rgb(147, 161, 161)

static struct termemu temu;

static void render_row(int wid, int screen_row)
{
    char *line = termemu_get_visible_row(&temu, screen_row);
    char buf[TERM_COLS + 1];
    int c;
    for (c = 0; c < TERM_COLS; c++) {
        char ch = line[c];
        buf[c] = (ch >= 32) ? ch : ' ';
    }
    buf[TERM_COLS] = '\0';

    unsigned int y = (unsigned int)(screen_row * FONT_H);
    win_fill_rect(xfd, wid, 0, y, CLIENT_W, FONT_H, COL_BG);
    win_draw_text(xfd, wid, 0, y, buf, COL_FG, COL_BG);

    if (temu.scroll_offset == 0 && screen_row == temu.cur_row) {
        unsigned int cx = (unsigned int)(temu.cur_col * FONT_W);
        char cell[2] = { buf[temu.cur_col], '\0' };
        win_fill_rect(xfd, wid, cx, y, FONT_W, FONT_H, COL_FG);
        win_draw_text(xfd, wid, cx, y, cell, COL_BG, COL_FG);
    }
}

static void render_dirty(int wid)
{
    int any = 0;
    int r;
    for (r = 0; r < temu.rows; r++) {
        if (termemu_is_dirty(&temu, r)) {
            render_row(wid, r);
            termemu_clear_dirty(&temu, r);
            any = 1;
        }
    }
    if (any)
        win_flip(xfd, wid);
}

static void render_all(int wid)
{
    termemu_mark_all_dirty(&temu);
    render_dirty(wid);
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    int shell_in[2];
    int shell_out[2];
    char readbuf[256];
    struct gui_event ev;
    int running = 1;
    int n;
    int i;

    if (pipe(shell_in) < 0 || pipe(shell_out) < 0)
        return 1;

    int child = fork();
    if (child < 0)
        return 1;

    if (child == 0) {
        dup2(shell_in[0],  0);
        dup2(shell_out[1], 1);
        dup2(shell_out[1], 2);
        vfs_close(shell_in[0]);
        vfs_close(shell_in[1]);
        vfs_close(shell_out[0]);
        vfs_close(shell_out[1]);

        char *sh_argv[] = { "/bin/sh", (char *)0 };
        execv("/bin/sh", sh_argv);
        exit(1);
    }

    int stdin_wr  = shell_in[1];
    int stdout_rd = shell_out[0];

    xfd = open("/dev/x", O_RDWR);
    if (xfd < 0) {
        vfs_close(stdin_wr);
        vfs_close(stdout_rd);
        return 1;
    }

    int wid = win_create(xfd, 40, 40, CLIENT_W + GUI_CHROME_W, CLIENT_H + GUI_CHROME_H, "Terminal");
    if (wid < 0) {
        vfs_close(stdin_wr);
        vfs_close(stdout_rd);
        return 1;
    }

    termemu_init(&temu, TERM_COLS, TERM_ROWS, TERMEMU_MAX_SB);

    win_fill_rect(xfd, wid, 0, 0, CLIENT_W, CLIENT_H, COL_BG);
    win_flip(xfd, wid);

    while (running) {
        /* Drain all available shell output before rendering */
        n = read_nb(stdout_rd, readbuf, sizeof(readbuf) - 1);
        if (n > 0) {
            do {
                for (i = 0; i < n; i++)
                    termemu_putchar(&temu, readbuf[i]);
                n = read_nb(stdout_rd, readbuf, sizeof(readbuf) - 1);
            } while (n > 0);
            render_dirty(wid);
        }

        /* Handle GUI events — drain all pending events before rendering */
        int scrolled = 0;
        while (win_poll_event(xfd, wid, &ev)) {
            if (ev.type == GUI_EVENT_CLOSE) {
                running = 0;
                break;
            }
            if (ev.type == GUI_EVENT_KEYPRESS && ev.key) {
                unsigned char ch = (unsigned char)ev.key;

                if (ch == 0x01) {           /* KEY_PGUP — scroll back */
                    termemu_scroll_up(&temu);
                    scrolled = 1;
                } else if (ch == 0x02) {    /* KEY_PGDN — scroll forward */
                    termemu_scroll_down(&temu);
                    scrolled = 1;
                } else if (ch == 0x10) {    /* KEY_UP */
                    write(stdin_wr, "\x1b[A", 3);
                } else if (ch == 0x11) {    /* KEY_DOWN */
                    write(stdin_wr, "\x1b[B", 3);
                } else if (ch == 0x12) {    /* KEY_LEFT */
                    write(stdin_wr, "\x1b[D", 3);
                } else if (ch == 0x13) {    /* KEY_RIGHT */
                    write(stdin_wr, "\x1b[C", 3);
                } else if (ch == 0x14) {    /* KEY_HOME */
                    write(stdin_wr, "\x1b[H", 3);
                } else if (ch == 0x15) {    /* KEY_END */
                    write(stdin_wr, "\x1b[F", 3);
                } else if (ch == 0x16) {    /* KEY_DEL */
                    write(stdin_wr, "\x1b[3~", 4);
                } else {
                    char c = (char)ch;
                    write(stdin_wr, &c, 1);
                }
            }
        }
        if (scrolled)
            render_all(wid);

        if (n == 0) {
            if (task_done(child))
                running = 0;
            else
                yield();
        }
    }

    vfs_close(stdin_wr);
    vfs_close(stdout_rd);
    win_destroy(xfd, wid);
    vfs_close(xfd);
    return 0;
}
