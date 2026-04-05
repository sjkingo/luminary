/* term.c — terminal emulator window running /bin/sh
 *
 * Creates a GUI window, forks /bin/sh with its stdin/stdout connected via
 * pipes, then runs an event loop:
 *   - GUI keypress events  → write to shell stdin pipe
 *   - shell stdout pipe    → parse and render to window (non-blocking read)
 *
 * Font: 8×16 (matches kernel fbdev).  Default size: 80×24 cells.
 * Scrollback: 512 lines, Page Up/Down to navigate.
 */

#include "syscall.h"
#include "x.h"
#include "libc/stdio.h"

static int xfd = -1;

#define TERM_COLS   80
#define TERM_ROWS   24
#define FONT_W       8
#define FONT_H      16
#define CLIENT_W    (TERM_COLS * FONT_W)
#define CLIENT_H    (TERM_ROWS * FONT_H)

#define SB_LINES    512

/* Solarized Dark colours */
#define COL_BG      rgb(  0,  26,  33)
#define COL_FG      rgb(147, 161, 161)
#define COL_CURSOR  rgb(147, 161, 161)

static char sb[SB_LINES][TERM_COLS];  /* ring of full-width lines */
static int  sb_head  = 0;             /* index of oldest line (or next to write when full) */
static int  sb_count = 0;             /* total lines in ring (0..SB_LINES) */

static char sb_blank[TERM_COLS];      /* always-blank row for padding */

/* scroll_offset: 0 = showing live bottom; N = scrolled back N rows */
static int scroll_offset = 0;

/* cursor position within the live bottom TERM_ROWS */
static int cur_col = 0;
static int cur_row = 0;   /* 0 = top of live area; grows until TERM_ROWS-1 */
/* Return a pointer to live row r (0 = top of live area, cur_row = bottom) */
static char *sb_live_row(int r)
{
    /* Live rows are the last (cur_row+1) lines in the ring.
     * When sb_count <= TERM_ROWS the live area hasn't filled the screen yet;
     * base is always the first line in the ring in that case. */
    int live   = sb_count < TERM_ROWS ? sb_count : TERM_ROWS;
    int base   = sb_count - live;
    int idx    = (sb_head + base + r) % SB_LINES;
    return sb[idx];
}

/* Return a pointer to scrollback row r counted from the top of the visible
 * window (which may be scrolled back by scroll_offset rows).
 * When fewer than TERM_ROWS lines exist the content is top-aligned. */
static char *visible_row(int r)
{
    if (sb_count == 0)
        return sb[0];   /* shouldn't happen but guard */

    /* How many lines are we showing from the ring? */
    int live = sb_count < TERM_ROWS ? sb_count : TERM_ROWS;

    /* vtop: index into the ring for the topmost visible row */
    int vtop;
    if (sb_count <= TERM_ROWS) {
        /* Screen not yet full — top-align; rows beyond live content are blank */
        vtop = 0;
    } else {
        vtop = sb_count - TERM_ROWS - scroll_offset;
        if (vtop < 0) vtop = 0;
    }

    /* Rows beyond live content (bottom padding when screen isn't full) */
    if (r >= live && sb_count < TERM_ROWS)
        return sb_blank;

    int idx = (sb_head + vtop + r) % SB_LINES;
    return sb[idx];
}

/* Allocate a new blank line at the bottom of the scrollback ring */
static void sb_new_line(void)
{
    int idx;
    if (sb_count < SB_LINES) {
        idx = (sb_head + sb_count) % SB_LINES;
        sb_count++;
    } else {
        /* ring is full — overwrite oldest */
        idx     = sb_head;
        sb_head = (sb_head + 1) % SB_LINES;
    }
    int c;
    for (c = 0; c < TERM_COLS; c++)
        sb[idx][c] = ' ';
}

static char dirty[TERM_ROWS];

static void mark_all_dirty(void)
{
    int r;
    for (r = 0; r < TERM_ROWS; r++)
        dirty[r] = 1;
}
/* Advance to the next row.  If we haven't filled the screen yet, just grow
 * cur_row.  Once cur_row would exceed TERM_ROWS-1, allocate a new ring line
 * and keep cur_row pinned at TERM_ROWS-1 (scroll). */
static void advance_row(void)
{
    if (cur_row < TERM_ROWS - 1) {
        /* Screen not yet full — allocate the new line and move cursor down */
        sb_new_line();
        cur_row++;
    } else {
        /* Screen full — push a new line into the ring, cursor stays at bottom */
        sb_new_line();
    }
    mark_all_dirty();
}

/* Ensure there is at least one line in the ring for the cursor to write on */
static void ensure_line(void)
{
    if (sb_count == 0)
        sb_new_line();
}

static void term_putchar(char ch)
{
    /* Any new output snaps back to live view */
    if (scroll_offset != 0) {
        scroll_offset = 0;
        mark_all_dirty();
    }

    ensure_line();

    if (ch == '\n') {
        cur_col = 0;
        advance_row();
        return;
    }
    if (ch == '\r') {
        cur_col = 0;
        return;
    }
    if (ch == '\b') {
        if (cur_col > 0) {
            cur_col--;
            sb_live_row(cur_row)[cur_col] = ' ';
            dirty[cur_row] = 1;
        }
        return;
    }
    if (ch == '\t') {
        int next = (cur_col + 8) & ~7;
        while (cur_col < next && cur_col < TERM_COLS) {
            sb_live_row(cur_row)[cur_col] = ' ';
            dirty[cur_row] = 1;
            cur_col++;
        }
        if (cur_col >= TERM_COLS) {
            cur_col = 0;
            advance_row();
        }
        return;
    }
    if (ch < 32) return;

    if (cur_col >= TERM_COLS) {
        cur_col = 0;
        advance_row();
    }
    sb_live_row(cur_row)[cur_col] = ch;
    dirty[cur_row] = 1;
    cur_col++;
}
static void render_row(int wid, int row)
{
    char *line = visible_row(row);

    char buf[TERM_COLS + 1];
    int c;
    for (c = 0; c < TERM_COLS; c++) {
        char ch = line[c];
        buf[c] = (ch >= 32) ? ch : ' ';
    }
    buf[TERM_COLS] = '\0';

    unsigned int y = (unsigned int)(row * FONT_H);
    win_fill_rect(xfd, wid, 0, y, CLIENT_W, FONT_H, COL_BG);
    win_draw_text(xfd, wid, 0, y, buf, COL_FG, COL_BG);

    /* Draw block cursor on the cursor cell when in live view */
    if (scroll_offset == 0 && row == cur_row) {
        unsigned int cx = (unsigned int)(cur_col * FONT_W);
        char cell[2] = { buf[cur_col], '\0' };
        win_fill_rect(xfd, wid, cx, y, FONT_W, FONT_H, COL_FG);
        win_draw_text(xfd, wid, cx, y, cell, COL_BG, COL_FG);
    }
}

static void render_dirty(int wid)
{
    int any = 0;
    int r;
    for (r = 0; r < TERM_ROWS; r++) {
        if (dirty[r]) {
            render_row(wid, r);
            dirty[r] = 0;
            any = 1;
        }
    }
    if (any)
        win_flip(xfd, wid);
}

static void render_all(int wid)
{
    mark_all_dirty();
    render_dirty(wid);
}
int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    int shell_in[2], shell_out[2];
    if (pipe(shell_in) < 0 || pipe(shell_out) < 0)
        return 1;

    int child = fork();
    if (child < 0)
        return 1;

    if (child == 0) {
        dup2(shell_in[0],  0);
        dup2(shell_out[1], 1);
        dup2(shell_out[1], 2);

        char *sh_argv[] = { "/bin/sh", (char *)0 };
        execv("/bin/sh", sh_argv);
        printf("term: exec failed\n");
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

    win_fill_rect(xfd, wid, 0, 0, CLIENT_W, CLIENT_H, COL_BG);
    win_flip(xfd, wid);

    char readbuf[256];
    struct gui_event ev;
    int running = 1;

    while (running) {
        /* Drain shell output */
        int n = read_nb(stdout_rd, readbuf, sizeof(readbuf) - 1);
        if (n > 0) {
            int i;
            for (i = 0; i < n; i++)
                term_putchar(readbuf[i]);
            render_dirty(wid);
        }

        /* Handle GUI events */
        while (win_poll_event(xfd, wid, &ev)) {
            if (ev.type == GUI_EVENT_CLOSE) {
                running = 0;
                break;
            }
            if (ev.type == GUI_EVENT_KEYPRESS && ev.key) {
                char ch = ev.key;

                /* Page Up / Page Down for scrollback */
                if (ch == '\x01') {   /* Page Up */
                    int max_scroll = sb_count - TERM_ROWS;
                    if (max_scroll < 0) max_scroll = 0;
                    scroll_offset += TERM_ROWS;
                    if (scroll_offset > max_scroll)
                        scroll_offset = max_scroll;
                    render_all(wid);
                } else if (ch == '\x02') {   /* Page Down */
                    scroll_offset -= TERM_ROWS;
                    if (scroll_offset < 0)
                        scroll_offset = 0;
                    render_all(wid);
                } else {
                    write(stdin_wr, &ch, 1);
                }
            }
        }

        if (n == 0)
            yield();
    }

    vfs_close(stdin_wr);
    vfs_close(stdout_rd);
    win_destroy(xfd, wid);
    vfs_close(xfd);
    return 0;
}
