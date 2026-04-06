/* readline.c — line editor with history. */

#include "libc/readline.h"
#include "libc/string.h"
#include "syscall.h"

/* Output helpers */
static void rl_write(const char *buf, int n)
{
    write(1, buf, (unsigned int)n);
}

static void rl_puts(const char *s)
{
    int n = 0;
    while (s[n]) n++;
    rl_write(s, n);
}

/* Reprint the line from scratch using CR.
 * Emits: \r + prompt + buf[0..len-1] + \x1b[K (erase to EOL)
 * then positions cursor via \x1b[nD if cursor < len. */
static void reprint(struct readline_state *rl, const char *prompt)
{
    char tmp[16];
    int backs = rl->len - rl->cursor;
    int i;

    rl_write("\r", 1);
    rl_puts(prompt);
    rl_write(rl->buf, rl->len);
    rl_write("\x1b[K", 3);  /* erase any leftover chars to the right */

    if (backs > 0) {
        /* \x1b[nD — move cursor left by n */
        tmp[0] = '\x1b'; tmp[1] = '[';
        i = 2;
        if (backs >= 100) { tmp[i++] = (char)('0' + backs / 100); }
        if (backs >= 10)  { tmp[i++] = (char)('0' + (backs / 10) % 10); }
        tmp[i++] = (char)('0' + backs % 10);
        tmp[i++] = 'D';
        rl_write(tmp, i);
    }
}

void readline_init(struct readline_state *rl,
                   char *buf, int bufsize,
                   char *hist_buf, int hist_max, int hist_len)
{
    rl->buf      = buf;
    rl->bufsize  = bufsize;
    rl->len      = 0;
    rl->cursor   = 0;
    rl->esc      = 0;
    rl->csi_param = 0;
    rl->csi_final = 0;
    rl->hist_buf  = hist_buf;
    rl->hist_max  = hist_max;
    rl->hist_len  = hist_len;
    rl->hist_head  = 0;
    rl->hist_count = 0;
    rl->hist_idx   = -1;
    rl->hist_save_len = 0;
}

void readline_history_push(struct readline_state *rl, const char *line, int len)
{
    int last;
    int is_dup;
    int slot;
    int i;

    if (!rl->hist_buf || len <= 0 || len >= rl->hist_len)
        return;

    /* Ignore consecutive duplicate */
    is_dup = 0;
    if (rl->hist_count > 0) {
        last = (rl->hist_head + rl->hist_count - 1) % rl->hist_max;
        char *prev = rl->hist_buf + last * rl->hist_len;
        is_dup = 1;
        for (i = 0; i <= len; i++) {
            if (prev[i] != line[i]) { is_dup = 0; break; }
        }
    }
    if (is_dup) return;

    if (rl->hist_count < rl->hist_max) {
        slot = (rl->hist_head + rl->hist_count) % rl->hist_max;
        rl->hist_count++;
    } else {
        slot = rl->hist_head;
        rl->hist_head = (rl->hist_head + 1) % rl->hist_max;
    }
    char *dst = rl->hist_buf + slot * rl->hist_len;
    for (i = 0; i <= len; i++)
        dst[i] = line[i];
}

/* Load history entry hist_idx into buf, update len and cursor. */
static void hist_load(struct readline_state *rl, const char *prompt)
{
    char *src = rl->hist_buf + rl->hist_idx * rl->hist_len;
    int n = 0;
    while (src[n] && n < rl->bufsize - 1) n++;
    int i;
    for (i = 0; i < n; i++) rl->buf[i] = src[i];
    rl->len = n;
    rl->cursor = n;
    reprint(rl, prompt);
}

static void handle_csi(struct readline_state *rl, char final,
                        const char *prompt)
{
    int n = rl->csi_param > 0 ? rl->csi_param : 1;
    int i;

    /* Two-part sequences like \x1b[3~ */
    if (final == '~') {
        if (rl->csi_final == '3') {  /* Delete key */
            if (rl->cursor < rl->len) {
                for (i = rl->cursor; i < rl->len - 1; i++)
                    rl->buf[i] = rl->buf[i + 1];
                rl->len--;
                reprint(rl, prompt);
            }
        }
        return;
    }

    switch (final) {
    case 'A':   /* Up — prev history */
        if (!rl->hist_buf || rl->hist_count == 0) return;
        if (rl->hist_idx < 0) {
            rl->hist_save_len = rl->len;
            rl->hist_idx = rl->hist_count;
        }
        if (rl->hist_idx > 0) {
            rl->hist_idx--;
            hist_load(rl, prompt);
        }
        break;
    case 'B':   /* Down — next history / blank */
        if (!rl->hist_buf || rl->hist_idx < 0) return;
        rl->hist_idx++;
        if (rl->hist_idx >= rl->hist_count) {
            rl->hist_idx = -1;
            rl->len = 0;
            rl->cursor = 0;
            reprint(rl, prompt);
        } else {
            hist_load(rl, prompt);
        }
        break;
    case 'D':   /* Left */
        if (rl->cursor > 0) {
            rl->cursor -= n;
            if (rl->cursor < 0) rl->cursor = 0;
            reprint(rl, prompt);
        }
        break;
    case 'C':   /* Right */
        if (rl->cursor < rl->len) {
            rl->cursor += n;
            if (rl->cursor > rl->len) rl->cursor = rl->len;
            reprint(rl, prompt);
        }
        break;
    case 'H':   /* Home */
        if (rl->cursor > 0) {
            rl->cursor = 0;
            reprint(rl, prompt);
        }
        break;
    case 'F':   /* End */
        if (rl->cursor < rl->len) {
            rl->cursor = rl->len;
            reprint(rl, prompt);
        }
        break;
    default:
        (void)n;
        break;
    }
}

/* Handle a fully decoded logical key action, sharing logic between
 * the ANSI path and the sentinel byte path. */
static int handle_sentinel(struct readline_state *rl, unsigned char s,
                            const char *prompt)
{
    int i;
    switch (s) {
    case 0x10:  /* KEY_UP */
        handle_csi(rl, 'A', prompt);
        break;
    case 0x11:  /* KEY_DOWN */
        handle_csi(rl, 'B', prompt);
        break;
    case 0x12:  /* KEY_LEFT */
        rl->csi_param = 0;
        handle_csi(rl, 'D', prompt);
        break;
    case 0x13:  /* KEY_RIGHT */
        rl->csi_param = 0;
        handle_csi(rl, 'C', prompt);
        break;
    case 0x14:  /* KEY_HOME */
        handle_csi(rl, 'H', prompt);
        break;
    case 0x15:  /* KEY_END */
        handle_csi(rl, 'F', prompt);
        break;
    case 0x16:  /* KEY_DEL */
        if (rl->cursor < rl->len) {
            for (i = rl->cursor; i < rl->len - 1; i++)
                rl->buf[i] = rl->buf[i + 1];
            rl->len--;
            reprint(rl, prompt);
        }
        break;
    default:
        return 0;
    }
    return 1;
}

int readline_readline(struct readline_state *rl, const char *prompt)
{
    char c;
    int i;

    rl->len    = 0;
    rl->cursor = 0;
    rl->esc    = 0;
    rl->hist_idx = -1;

    rl_puts(prompt);

    for (;;) {
        if (read(0, &c, 1) == 0)
            continue;

        /* ANSI escape state machine */
        if (rl->esc == 1) {
            if (c == '[') { rl->esc = 2; rl->csi_param = 0; rl->csi_final = 0; }
            else rl->esc = 0;
            continue;
        }
        if (rl->esc == 2) {
            if (c >= '0' && c <= '9') {
                rl->csi_param = rl->csi_param * 10 + (c - '0');
                rl->csi_final = c;
                continue;
            }
            rl->esc = 0;
            if (c >= 0x40 && c <= 0x7E) {
                /* csi_final holds the last digit byte for two-part seqs */
                handle_csi(rl, c, prompt);
            }
            continue;
        }
        if (c == '\x1b') { rl->esc = 1; continue; }

        /* Ctrl+C */
        if (c == '\x03') {
            rl->len = 0; rl->cursor = 0;
            rl->buf[0] = '\0';
            rl_write("^C\n", 3);
            return RL_CTRLC;
        }

        /* Enter */
        if (c == '\n') {
            rl->buf[rl->len] = '\0';
            rl_write("\n", 1);
            rl->hist_idx = -1;
            return rl->len;
        }

        /* Backspace */
        if (c == '\b' || c == '\x7f') {
            if (rl->cursor > 0) {
                rl->cursor--;
                for (i = rl->cursor; i < rl->len - 1; i++)
                    rl->buf[i] = rl->buf[i + 1];
                rl->len--;
                reprint(rl, prompt);
            }
            continue;
        }

        /* Sentinel bytes from fbcon keyboard path */
        if ((unsigned char)c >= 0x10 && (unsigned char)c <= 0x16) {
            handle_sentinel(rl, (unsigned char)c, prompt);
            continue;
        }

        /* Discard other control chars */
        if ((unsigned char)c < 32) continue;

        /* Insert printable character at cursor */
        if (rl->len < rl->bufsize - 1) {
            for (i = rl->len; i > rl->cursor; i--)
                rl->buf[i] = rl->buf[i - 1];
            rl->buf[rl->cursor] = c;
            rl->len++;
            rl->cursor++;
            reprint(rl, prompt);
        }
    }
}
