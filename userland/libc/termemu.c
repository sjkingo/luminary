/* termemu.c — terminal emulator ring buffer and character dispatch.
 *
 * Implements scrollback ring, cursor tracking, and character dispatch
 * (\n, \r, \b, \t, auto-wrap).  Rendering is left to the caller via
 * termemu_get_visible_row / termemu_get_live_row + the dirty flags.
 *
 * Tab stops: 8 columns (POSIX default).
 */

#include "libc/termemu.h"
#include "libc/stdlib.h"
#include "libc/string.h"

void termemu_init(struct termemu *t, int cols, int rows, int sb_lines)
{
    int r;

    if (cols > TERMEMU_MAX_COLS) cols = TERMEMU_MAX_COLS;
    if (rows > TERMEMU_MAX_ROWS) rows = TERMEMU_MAX_ROWS;
    if (sb_lines > TERMEMU_MAX_SB) sb_lines = TERMEMU_MAX_SB;

    t->cols      = cols;
    t->rows      = rows;
    t->sb_lines  = sb_lines;
    t->sb_head   = 0;
    t->sb_count  = 0;
    t->cur_col   = 0;
    t->cur_row   = 0;
    t->scroll_offset = 0;
    t->esc       = 0;
    t->csi_param = 0;

    t->sb    = (char *)malloc((unsigned int)(sb_lines * cols));
    t->dirty = (char *)malloc((unsigned int)rows);

    for (r = 0; r < sb_lines; r++)
        memset(t->sb + r * cols, ' ', (unsigned int)cols);
    for (r = 0; r < rows; r++)
        t->dirty[r] = 0;

    /* Start with one blank live line */
    t->sb_count = 1;
}

static void sb_new_line(struct termemu *t)
{
    int idx;
    if (t->sb_count < t->sb_lines) {
        idx = (t->sb_head + t->sb_count) % t->sb_lines;
        t->sb_count++;
    } else {
        idx       = t->sb_head;
        t->sb_head = (t->sb_head + 1) % t->sb_lines;
    }
    memset(t->sb + idx * t->cols, ' ', (unsigned int)t->cols);
}

static void advance_row(struct termemu *t)
{
    if (t->cur_row < t->rows - 1) {
        sb_new_line(t);
        t->cur_row++;
        /* Only the new row needs clearing; existing rows are unchanged */
        t->dirty[t->cur_row] = 1;
    } else {
        /* Terminal full — ring advances, every visible row shifts up */
        sb_new_line(t);
        termemu_mark_all_dirty(t);
    }
}

static void csi_dispatch(struct termemu *t, char final)
{
    int n = t->csi_param > 0 ? t->csi_param : 1;
    int c;
    char *row;

    switch (final) {
    case 'D':   /* cursor left n */
        c = t->cur_col - n;
        t->cur_col = c < 0 ? 0 : c;
        t->dirty[t->cur_row] = 1;
        break;
    case 'C':   /* cursor right n */
        c = t->cur_col + n;
        t->cur_col = c >= t->cols ? t->cols - 1 : c;
        t->dirty[t->cur_row] = 1;
        break;
    case 'A':   /* cursor up n */
        c = t->cur_row - n;
        t->cur_row = c < 0 ? 0 : c;
        t->dirty[t->cur_row] = 1;
        break;
    case 'B':   /* cursor down n */
        c = t->cur_row + n;
        t->cur_row = c >= t->rows ? t->rows - 1 : c;
        t->dirty[t->cur_row] = 1;
        break;
    case 'K':   /* erase in line: 0=to end, 1=to start, 2=whole */
        row = termemu_get_live_row(t, t->cur_row);
        if (t->csi_param == 1) {
            int i;
            for (i = 0; i <= t->cur_col && i < t->cols; i++)
                row[i] = ' ';
        } else if (t->csi_param == 2) {
            memset(row, ' ', (unsigned int)t->cols);
        } else {
            int i;
            for (i = t->cur_col; i < t->cols; i++)
                row[i] = ' ';
        }
        t->dirty[t->cur_row] = 1;
        break;
    case 'G':   /* cursor to column n (1-based) */
        c = (t->csi_param > 0 ? t->csi_param : 1) - 1;
        t->cur_col = c < 0 ? 0 : (c >= t->cols ? t->cols - 1 : c);
        t->dirty[t->cur_row] = 1;
        break;
    case 'P': { /* delete n chars at cursor (shift left) */
        int i;
        row = termemu_get_live_row(t, t->cur_row);
        for (i = t->cur_col; i < t->cols - n; i++)
            row[i] = row[i + n];
        for (i = t->cols - n; i < t->cols; i++)
            row[i] = ' ';
        t->dirty[t->cur_row] = 1;
        break;
    }
    default:
        break;
    }
}

void termemu_putchar(struct termemu *t, char ch)
{
    char *row;

    /* CSI escape state machine */
    if (t->esc == 1) {
        if (ch == '[') { t->esc = 2; t->csi_param = 0; return; }
        t->esc = 0;
        return; /* unrecognised ESC sequence — discard */
    }
    if (t->esc == 2) {
        if (ch >= '0' && ch <= '9') {
            t->csi_param = t->csi_param * 10 + (ch - '0');
            return;
        }
        t->esc = 0;
        if (ch >= 0x40 && ch <= 0x7E)
            csi_dispatch(t, ch);
        return;
    }
    if (ch == '\x1b') {
        /* Any new escape snaps back to live view */
        if (t->scroll_offset != 0) {
            t->scroll_offset = 0;
            termemu_mark_all_dirty(t);
        }
        t->esc = 1;
        return;
    }

    /* Any new output snaps back to live view */
    if (t->scroll_offset != 0) {
        t->scroll_offset = 0;
        termemu_mark_all_dirty(t);
    }

    /* Ensure at least one line exists */
    if (t->sb_count == 0)
        sb_new_line(t);

    if (ch == '\n') {
        t->dirty[t->cur_row] = 1;  /* erase cursor from old row */
        t->cur_col = 0;
        advance_row(t);
        return;
    }
    if (ch == '\r') {
        t->dirty[t->cur_row] = 1;
        t->cur_col = 0;
        return;
    }
    if (ch == '\b') {
        if (t->cur_col > 0) {
            t->cur_col--;
            t->dirty[t->cur_row] = 1;
        }
        return;
    }
    if (ch == '\t') {
        int next = (t->cur_col + 8) & ~7;
        while (t->cur_col < next && t->cur_col < t->cols) {
            termemu_get_live_row(t, t->cur_row)[t->cur_col] = ' ';
            t->dirty[t->cur_row] = 1;
            t->cur_col++;
        }
        if (t->cur_col >= t->cols) {
            t->cur_col = 0;
            advance_row(t);
        }
        return;
    }

    if (ch < 32) return;   /* discard other control chars */

    if (t->cur_col >= t->cols) {
        t->cur_col = 0;
        advance_row(t);
    }

    row = termemu_get_live_row(t, t->cur_row);
    row[t->cur_col] = ch;
    t->dirty[t->cur_row] = 1;
    t->cur_col++;
}

void termemu_scroll_up(struct termemu *t)
{
    int max_scroll = t->sb_count - t->rows;
    if (max_scroll < 0) max_scroll = 0;
    t->scroll_offset += t->rows;
    if (t->scroll_offset > max_scroll)
        t->scroll_offset = max_scroll;
    termemu_mark_all_dirty(t);
}

void termemu_scroll_down(struct termemu *t)
{
    t->scroll_offset -= t->rows;
    if (t->scroll_offset < 0)
        t->scroll_offset = 0;
    termemu_mark_all_dirty(t);
}

char *termemu_get_live_row(struct termemu *t, int r)
{
    int live = t->sb_count < t->rows ? t->sb_count : t->rows;
    int base = t->sb_count - live;
    int idx  = (t->sb_head + base + r) % t->sb_lines;
    return t->sb + idx * t->cols;
}

char *termemu_get_visible_row(struct termemu *t, int r)
{
    static char blank[TERMEMU_MAX_COLS];
    int vtop;
    int idx;

    if (t->sb_count == 0)
        return blank;

    /* Rows past the available content are blank */
    if (r >= t->sb_count)
        return blank;

    if (t->sb_count <= t->rows) {
        vtop = 0;
    } else {
        vtop = t->sb_count - t->rows - t->scroll_offset;
        if (vtop < 0) vtop = 0;
    }

    idx = (t->sb_head + vtop + r) % t->sb_lines;
    return t->sb + idx * t->cols;
}

int termemu_is_dirty(struct termemu *t, int row)
{
    return t->dirty[row];
}

void termemu_clear_dirty(struct termemu *t, int row)
{
    t->dirty[row] = 0;
}

void termemu_mark_all_dirty(struct termemu *t)
{
    int r;
    for (r = 0; r < t->rows; r++)
        t->dirty[r] = 1;
}
