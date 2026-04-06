/* readline.h — line editor with history.
 *
 * Generic enough to use from any program that reads interactive text input.
 * Supports:
 *   - Insert mode: type anywhere in the line
 *   - Left/Right arrow: non-destructive cursor movement (VT100 \x1b[D / \x1b[C)
 *   - Home/End: jump to start/end of line
 *   - Backspace: delete char before cursor
 *   - Delete (KEY_DEL / \x1b[3~): delete char under cursor
 *   - Up/Down arrow: history navigation
 *   - Ctrl+C: clear line, returns RL_CTRLC
 *
 * Input is read one byte at a time from fd 0. Output goes to fd 1.
 * Both ANSI escape sequences (\x1b[...) and sentinel bytes (0x10-0x16)
 * from the fbcon keyboard path are handled.
 *
 * History is a flat caller-owned buffer: hist_buf[hist_max][hist_len].
 * The caller allocates it (e.g. via malloc(hist_max * hist_len)) and passes
 * the pointer along with dimensions. readline owns the ring indices.
 */
#pragma once

#define RL_CTRLC  (-2)   /* Ctrl+C was pressed — line cleared */

struct readline_state {
    /* line buffer */
    char *buf;
    int   bufsize;
    int   len;      /* current line length */
    int   cursor;   /* insertion point (0..len) */

    /* ANSI/CSI parser */
    int   esc;
    int   csi_param;
    char  csi_final; /* final byte of a two-part sequence (e.g. '3' before '~') */

    /* history ring — caller-owned flat buffer */
    char *hist_buf;  /* hist_buf[hist_max][hist_len] */
    int   hist_max;
    int   hist_len;
    int   hist_head;
    int   hist_count;
    int   hist_idx;  /* -1 = not browsing */
    int   hist_save_len; /* saved len before history browse started */
};

/* Initialise state. hist_buf may be NULL to disable history. */
void readline_init(struct readline_state *rl,
                   char *buf, int bufsize,
                   char *hist_buf, int hist_max, int hist_len);

/* Read one complete line.
 * Displays prompt, then processes input until Enter.
 * Returns line length (>=0), or RL_CTRLC if Ctrl+C was pressed.
 * On return, rl->buf is NUL-terminated and rl->len reflects the length. */
int readline_readline(struct readline_state *rl, const char *prompt);

/* Push a string into the history ring (ignores consecutive duplicates). */
void readline_history_push(struct readline_state *rl, const char *line, int len);
