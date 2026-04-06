/* termemu.h — shared userland terminal emulator state.
 *
 * Encapsulates scrollback ring buffer, cursor, and dirty-row tracking.
 * Used by both term.c (GUI window rendering) and fbcon.c (framebuffer).
 */
#pragma once

#define TERMEMU_MAX_COLS   256
#define TERMEMU_MAX_ROWS   128
#define TERMEMU_MAX_SB    1024  /* max scrollback lines */

struct termemu {
    /* ring buffer: sb[SB_LINES][cols], allocated statically at max size */
    char sb[TERMEMU_MAX_SB][TERMEMU_MAX_COLS];
    int  sb_head;   /* index of oldest line (or next to overwrite when full) */
    int  sb_count;  /* total lines in ring (0..sb_lines) */

    /* configured dimensions */
    int  cols;
    int  rows;
    int  sb_lines;  /* scrollback capacity (<=TERMEMU_MAX_SB) */

    /* cursor within the live bottom `rows` lines */
    int  cur_col;
    int  cur_row;   /* 0 = top of live area; grows until rows-1 */

    /* scrollback view: 0 = live bottom, N = scrolled back N rows */
    int  scroll_offset;

    /* dirty flags for incremental rendering */
    char dirty[TERMEMU_MAX_ROWS];
};

void termemu_init(struct termemu *t, int cols, int rows, int sb_lines);
void termemu_putchar(struct termemu *t, char ch);
void termemu_scroll_up(struct termemu *t);
void termemu_scroll_down(struct termemu *t);

/* Returns pointer to the visible line for screen row r (0=top).
 * Accounts for scroll_offset. */
char *termemu_get_visible_row(struct termemu *t, int r);

/* Returns pointer to a specific live row (0=top of live area). */
char *termemu_get_live_row(struct termemu *t, int r);

int  termemu_is_dirty(struct termemu *t, int row);
void termemu_clear_dirty(struct termemu *t, int row);
void termemu_mark_all_dirty(struct termemu *t);
