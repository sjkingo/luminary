/* term.c — terminal emulator window running /bin/sh
 *
 * Creates a GUI window, forks /bin/sh with its stdin/stdout connected via
 * pipes, then runs an event loop:
 *   - GUI keypress events  → write to shell stdin pipe
 *   - shell stdout pipe    → parse and render to window (non-blocking read)
 *
 * Font: 8×16 (matches kernel fbdev).  Default size: 80×24 cells.
 */

#include "syscall.h"
#include "gui.h"

/* ── terminal dimensions ─────────────────────────────────────────────────── */
#define TERM_COLS   80
#define TERM_ROWS   24
#define FONT_W       8
#define FONT_H      16
#define CLIENT_W    (TERM_COLS * FONT_W)   /* 640 */
#define CLIENT_H    (TERM_ROWS * FONT_H)   /* 384 */

/* ── Solarized Dark colours (match fbdev console) ────────────────────────── */
#define COL_BG      rgb(  0,  26,  33)
#define COL_FG      rgb(147, 161, 161)
#define COL_CURSOR  rgb(147, 161, 161)

/* ── cell buffer ─────────────────────────────────────────────────────────── */
static char cells[TERM_ROWS][TERM_COLS];
static char dirty[TERM_ROWS];   /* 1 = row needs redraw */

static int cur_col = 0;
static int cur_row = 0;

static void mark_dirty(int row)
{
    if (row >= 0 && row < TERM_ROWS)
        dirty[row] = 1;
}

static void scroll_up(void)
{
    int r;
    for (r = 0; r < TERM_ROWS - 1; r++) {
        int c;
        for (c = 0; c < TERM_COLS; c++)
            cells[r][c] = cells[r + 1][c];
        mark_dirty(r);
    }
    for (int c = 0; c < TERM_COLS; c++)
        cells[TERM_ROWS - 1][c] = ' ';
    mark_dirty(TERM_ROWS - 1);
}

static void term_putchar(char ch)
{
    if (ch == '\n') {
        cur_col = 0;
        cur_row++;
        if (cur_row >= TERM_ROWS) {
            scroll_up();
            cur_row = TERM_ROWS - 1;
        }
        return;
    }
    if (ch == '\r') {
        cur_col = 0;
        return;
    }
    if (ch == '\b') {
        if (cur_col > 0) {
            cur_col--;
            cells[cur_row][cur_col] = ' ';
            mark_dirty(cur_row);
        }
        return;
    }
    if (ch == '\t') {
        int next = (cur_col + 8) & ~7;
        while (cur_col < next && cur_col < TERM_COLS) {
            cells[cur_row][cur_col] = ' ';
            mark_dirty(cur_row);
            cur_col++;
        }
        if (cur_col >= TERM_COLS) {
            cur_col = 0;
            cur_row++;
            if (cur_row >= TERM_ROWS) {
                scroll_up();
                cur_row = TERM_ROWS - 1;
            }
        }
        return;
    }
    /* Printable */
    if (ch < 32) return;  /* ignore other control chars */

    if (cur_col >= TERM_COLS) {
        cur_col = 0;
        cur_row++;
        if (cur_row >= TERM_ROWS) {
            scroll_up();
            cur_row = TERM_ROWS - 1;
        }
    }
    cells[cur_row][cur_col] = ch;
    mark_dirty(cur_row);
    cur_col++;
}

/* ── rendering ───────────────────────────────────────────────────────────── */

/* Draw a single row as a text string via win_draw_text.
 * win_draw_text renders one NUL-terminated string; we pass the whole row. */
static void render_row(int wid, int row)
{
    /* Build NUL-terminated string from cell row */
    char buf[TERM_COLS + 1];
    int c;
    /* Find last non-space to avoid trailing garbage; draw full row for simplicity */
    for (c = 0; c < TERM_COLS; c++) {
        char ch = cells[row][c];
        buf[c] = (ch >= 32) ? ch : ' ';
    }
    buf[TERM_COLS] = '\0';

    unsigned int y = (unsigned int)(row * FONT_H);
    /* Clear the row first */
    win_fill_rect(wid, 0, y, CLIENT_W, FONT_H, COL_BG);
    /* Draw text */
    win_draw_text(wid, 0, y, buf, COL_FG, COL_BG);
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
        win_flip(wid);
}

static void render_all(int wid)
{
    int r;
    for (r = 0; r < TERM_ROWS; r++)
        mark_dirty(r);
    render_dirty(wid);
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    /* pipes: shell_in[0] = read by shell (stdin), shell_in[1] = written by us
     *        shell_out[0] = read by us,           shell_out[1] = written by shell (stdout) */
    int shell_in[2], shell_out[2];
    if (pipe(shell_in) < 0 || pipe(shell_out) < 0)
        return 1;

    int child = fork();
    if (child < 0)
        return 1;

    if (child == 0) {
        /* Child: wire stdio to the pipes then exec the shell.
         * Do NOT close the other pipe ends — the pipe implementation has no
         * refcounting, so closing any endpoint sets a shared closed flag that
         * breaks the parent's side of the pipe. */
        dup2(shell_in[0],  0);   /* stdin  = read end of shell_in */
        dup2(shell_out[1], 1);   /* stdout = write end of shell_out */
        dup2(shell_out[1], 2);   /* stderr = same */

        char *sh_argv[] = { "/bin/sh", (char *)0 };
        execv("/bin/sh", sh_argv);
        /* execv only returns on failure */
        write(1, "term: exec failed\n", 18);
        exit(1);
    }

    /* Parent: do NOT close the unused pipe ends for the same reason —
     * pipe_notify_close sets a shared flag that would break the child's I/O. */

    int stdin_wr  = shell_in[1];   /* we write keypresses here */
    int stdout_rd = shell_out[0];  /* we read shell output here */

    /* Create the terminal window */
    int wid = win_create(40, 40, CLIENT_W, CLIENT_H, "Terminal");
    if (wid < 0) {
        vfs_close(stdin_wr);
        vfs_close(stdout_rd);
        return 1;
    }

    /* Init cell buffer to spaces */
    int r, c;
    for (r = 0; r < TERM_ROWS; r++) {
        for (c = 0; c < TERM_COLS; c++)
            cells[r][c] = ' ';
        dirty[r] = 0;
    }

    /* Paint initial background */
    win_fill_rect(wid, 0, 0, CLIENT_W, CLIENT_H, COL_BG);
    win_flip(wid);

    /* Event loop */
    char readbuf[256];
    struct gui_event ev;
    int running = 1;

    while (running) {
        /* Drain shell output (non-blocking) */
        int n = read_nb(stdout_rd, readbuf, sizeof(readbuf) - 1);
        if (n > 0) {
            int i;
            for (i = 0; i < n; i++)
                term_putchar(readbuf[i]);
            render_dirty(wid);
        }

        /* Handle GUI events */
        while (win_poll_event(wid, &ev)) {
            if (ev.type == GUI_EVENT_CLOSE) {
                running = 0;
                break;
            }
            if (ev.type == GUI_EVENT_KEYPRESS && ev.key) {
                char ch = ev.key;
                write(stdin_wr, &ch, 1);
            }
        }

        /* Yield CPU if nothing to do */
        if (n == 0)
            yield();
    }

    vfs_close(stdin_wr);
    vfs_close(stdout_rd);
    win_destroy(wid);
    return 0;
}
