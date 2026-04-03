/* Luminary GUI demo application.
 *
 * Demonstrates:
 *   - Multiple draggable windows
 *   - Buttons (click detection in client area)
 *   - A live uptime counter
 *   - Keyboard input into a text field
 */

#include "syscall.h"
#include "gui.h"

/* ── colours ─────────────────────────────────────────────────────────────── */
#define COL_WHITE   rgb(255, 255, 255)
#define COL_BLACK   rgb(0,   0,   0)
#define COL_GREY    rgb(200, 200, 200)
#define COL_DKGREY  rgb(120, 120, 120)
#define COL_BLUE    rgb(60,  90,  200)
#define COL_LBLUE   rgb(100, 140, 240)
#define COL_RED     rgb(200, 60,  60)
#define COL_LRED    rgb(240, 100, 100)
#define COL_GREEN   rgb(60,  180, 60)
#define COL_LGREEN  rgb(100, 220, 100)
#define COL_BGWIN   rgb(240, 240, 240)

/* ── font metrics (must match kernel 8×16 font) ──────────────────────────── */
#define FONT_W  8
#define FONT_H  16

/* ── minimal string utils ────────────────────────────────────────────────── */

static unsigned int mystrlen(const char *s)
{
    unsigned int n = 0;
    while (s[n]) n++;
    return n;
}

static void myutoa(unsigned int v, char *buf)
{
    char tmp[12];
    int i = 0;
    if (v == 0) { buf[0]='0'; buf[1]='\0'; return; }
    while (v) { tmp[i++] = '0' + (char)(v % 10); v /= 10; }
    int j = 0;
    while (i > 0) buf[j++] = tmp[--i];
    buf[j] = '\0';
}

/* Append src to dst (dst must be large enough) */
static void mystrcat(char *dst, const char *src)
{
    while (*dst) dst++;
    while (*src) *dst++ = *src++;
    *dst = '\0';
}

static void mystrcpy(char *dst, const char *src)
{
    while (*src) *dst++ = *src++;
    *dst = '\0';
}

/* ── button helper ───────────────────────────────────────────────────────── */

struct button {
    unsigned int x, y, w, h;
    const char *label;
    unsigned int col_normal;
    unsigned int col_hover;   /* unused for now — no hover tracking */
    int pressed;              /* set to 1 for one frame after click */
};

static void button_draw(int wid, struct button *btn)
{
    unsigned int col = btn->pressed ? btn->col_hover : btn->col_normal;
    win_fill_rect(wid, btn->x, btn->y, btn->w, btn->h, col);
    win_draw_rect(wid, btn->x, btn->y, btn->w, btn->h, COL_BLACK);

    /* Centre label text */
    unsigned int tw = mystrlen(btn->label) * FONT_W;
    unsigned int tx = btn->x + (btn->w > tw ? (btn->w - tw) / 2 : 0);
    unsigned int ty = btn->y + (btn->h > FONT_H ? (btn->h - FONT_H) / 2 : 0);
    win_draw_text(wid, tx, ty, btn->label, COL_BLACK, col);
}

/* Returns 1 if (x,y) is inside button */
static int button_hit(struct button *btn, unsigned int x, unsigned int y)
{
    return x >= btn->x && x < btn->x + btn->w &&
           y >= btn->y && y < btn->y + btn->h;
}

/* ── text field ──────────────────────────────────────────────────────────── */

#define TEXTFIELD_MAX 64

struct textfield {
    unsigned int x, y, w;
    char buf[TEXTFIELD_MAX];
    unsigned int len;
    int focused;
};

static void textfield_draw(int wid, struct textfield *tf)
{
    unsigned int h = FONT_H + 4;
    unsigned int border = tf->focused ? COL_BLUE : COL_DKGREY;
    win_fill_rect(wid, tf->x, tf->y, tf->w, h, COL_WHITE);
    win_draw_rect(wid, tf->x, tf->y, tf->w, h, border);

    /* Draw text + cursor */
    char display[TEXTFIELD_MAX + 2];
    mystrcpy(display, tf->buf);
    if (tf->focused) {
        display[tf->len]     = '_';
        display[tf->len + 1] = '\0';
    }
    win_draw_text(wid, tf->x + 3, tf->y + 2, display, COL_BLACK, COL_WHITE);
}

static void textfield_key(struct textfield *tf, char c)
{
    if (c == '\b') {
        if (tf->len > 0) tf->buf[--tf->len] = '\0';
    } else if (c >= 32 && c < 127 && tf->len < TEXTFIELD_MAX - 1) {
        tf->buf[tf->len++] = c;
        tf->buf[tf->len]   = '\0';
    }
}

/* ── windows ─────────────────────────────────────────────────────────────── */

/* Window 1: info panel with buttons */
static int win1 = -1;
static struct button btn_hello  = {10, 10, 100, 28, "Hello!",  0, 0, 0};
static struct button btn_clear  = {120, 10, 100, 28, "Clear",  0, 0, 0};
static char   label_text[128]  = "Click a button.";

/* Window 2: live uptime display */
static int win2 = -1;

/* Window 3: text input demo */
static int win3 = -1;
static struct textfield tfield  = {10, 40, 260, "", 0, 0};
static struct button btn_submit = {10, 76, 80, 24, "Submit", 0, 0, 0};
static char   submitted[TEXTFIELD_MAX] = "";

/* Window 4: console */
static int win4 = -1;

#define CON_COLS_MAX  80
#define CON_ROWS_MAX  40
#define CON_LINES     256   /* scrollback lines */
#define CON_INPUT_MAX 128

#define COL_CON_BG    rgb(20,  20,  20)
#define COL_CON_TEXT  rgb(200, 255, 200)
#define COL_CON_INPUT rgb(255, 255, 255)
#define COL_CON_PRMPT rgb(100, 200, 100)

static char  con_lines[CON_LINES][CON_COLS_MAX + 1];
static int   con_line_count = 0;   /* total lines written */
static char  con_input[CON_INPUT_MAX];
static int   con_input_len = 0;

/* Append a line to the console scrollback */
static void con_push_line(const char *s)
{
    int dst = con_line_count % CON_LINES;
    int i = 0;
    while (s[i] && i < CON_COLS_MAX) {
        con_lines[dst][i] = s[i];
        i++;
    }
    con_lines[dst][i] = '\0';
    con_line_count++;
}

/* Simple itoa for console built-ins */
static void con_utoa(unsigned int v, char *buf)
{
    char tmp[12]; int i = 0;
    if (v == 0) { buf[0] = '0'; buf[1] = '\0'; return; }
    while (v) { tmp[i++] = '0' + (char)(v % 10); v /= 10; }
    int j = 0;
    while (i > 0) buf[j++] = tmp[--i];
    buf[j] = '\0';
}

static void con_strcat(char *dst, const char *src)
{
    while (*dst) dst++;
    while (*src) *dst++ = *src++;
    *dst = '\0';
}

static int con_strcmp(const char *a, const char *b)
{
    while (*a && *b && *a == *b) { a++; b++; }
    return *a - *b;
}

/* Execute a command typed in the console */
static void con_exec(const char *cmd)
{
    /* Echo the command */
    char echo[CON_COLS_MAX + 4];
    echo[0] = '$'; echo[1] = ' ';
    int i = 0;
    while (cmd[i] && i < CON_COLS_MAX - 2) { echo[i+2] = cmd[i]; i++; }
    echo[i+2] = '\0';
    con_push_line(echo);

    if (con_strcmp(cmd, "") == 0) {
        return;
    } else if (con_strcmp(cmd, "clear") == 0) {
        con_line_count = 0;
    } else if (con_strcmp(cmd, "uptime") == 0) {
        char buf[32];
        unsigned int s = (unsigned int)uptime() / 1000;
        con_utoa(s, buf);
        con_strcat(buf, "s uptime");
        con_push_line(buf);
    } else if (con_strcmp(cmd, "help") == 0) {
        con_push_line("Commands: help, clear, uptime, pid");
    } else if (con_strcmp(cmd, "pid") == 0) {
        char buf[24];
        con_utoa((unsigned int)getpid(), buf);
        char out[32]; out[0]='\0';
        con_strcat(out, "pid: ");
        con_strcat(out, buf);
        con_push_line(out);
    } else {
        char out[CON_COLS_MAX];
        out[0] = '\0';
        con_strcat(out, "unknown: ");
        con_strcat(out, cmd);
        con_push_line(out);
    }
}

static void draw_win4(void)
{
    if (win4 < 0) return;

    unsigned int cw, ch;
    win_get_size(win4, &cw, &ch);
    if (cw == 0 || ch == 0) return;

    /* Fill background */
    win_fill_rect(win4, 0, 0, cw, ch, COL_CON_BG);

    /* How many text rows fit (leave 1 row for input line at bottom) */
    unsigned int rows = ch / FONT_H;
    if (rows == 0) { win_flip(win4); return; }
    unsigned int text_rows = rows > 1 ? rows - 1 : 0;
    unsigned int cols = cw / FONT_W;
    if (cols == 0) cols = 1;
    if (cols > CON_COLS_MAX) cols = CON_COLS_MAX;

    /* Draw scrollback lines, most recent at bottom of text area */
    int first = con_line_count - (int)text_rows;
    if (first < 0) first = 0;
    for (unsigned int r = 0; r < text_rows; r++) {
        int li = first + (int)r;
        if (li >= con_line_count) break;
        const char *line = con_lines[li % CON_LINES];
        unsigned int y = r * FONT_H;
        /* Draw char by char up to cols */
        for (unsigned int c = 0; line[c] && c < cols; c++) {
            char tmp[2] = { line[c], '\0' };
            win_draw_text(win4, c * FONT_W, y, tmp, COL_CON_TEXT, COL_CON_BG);
        }
    }

    /* Input line at bottom */
    unsigned int input_y = text_rows * FONT_H;
    win_fill_rect(win4, 0, input_y, cw, FONT_H, COL_CON_BG);
    /* Prompt */
    win_draw_text(win4, 0, input_y, "$ ", COL_CON_PRMPT, COL_CON_BG);
    /* Input text + cursor */
    char display[CON_INPUT_MAX + 2];
    int di = 0;
    for (int i = 0; i < con_input_len; i++) display[di++] = con_input[i];
    display[di++] = '_';
    display[di]   = '\0';
    win_draw_text(win4, 2 * FONT_W, input_y, display, COL_CON_INPUT, COL_CON_BG);

    win_flip(win4);
}

static void handle_win4_key(char c)
{
    if (c == '\r' || c == '\n') {
        con_input[con_input_len] = '\0';
        con_exec(con_input);
        con_input_len = 0;
        con_input[0]  = '\0';
    } else if (c == '\b') {
        if (con_input_len > 0) con_input[--con_input_len] = '\0';
    } else if (c >= 32 && c < 127 && con_input_len < CON_INPUT_MAX - 1) {
        con_input[con_input_len++] = c;
        con_input[con_input_len]   = '\0';
    }
    draw_win4();
}

static void handle_win4_event(struct gui_event *ev)
{
    if (ev->type == GUI_EVENT_KEYPRESS)
        handle_win4_key(ev->key);
    else if (ev->type == GUI_EVENT_RESIZE)
        draw_win4();
}

static void init_colours(void)
{
    btn_hello.col_normal  = COL_BLUE;
    btn_hello.col_hover   = COL_LBLUE;
    btn_clear.col_normal  = COL_RED;
    btn_clear.col_hover   = COL_LRED;
    btn_submit.col_normal = COL_GREEN;
    btn_submit.col_hover  = COL_LGREEN;
}

/* ── draw routines ───────────────────────────────────────────────────────── */

static void draw_win1(void)
{
    win_fill_rect(win1, 0, 0, 300, 200, COL_BGWIN);

    button_draw(win1, &btn_hello);
    button_draw(win1, &btn_clear);

    /* Label area */
    win_fill_rect(win1, 10, 50, 280, FONT_H + 4, COL_BGWIN);
    win_draw_text(win1, 10, 52, label_text, COL_BLACK, COL_BGWIN);

    win_flip(win1);

    /* Reset pressed state */
    btn_hello.pressed = 0;
    btn_clear.pressed = 0;
}

static void draw_win2(void)
{
    win_fill_rect(win2, 0, 0, 200, 80, COL_BGWIN);

    win_draw_text(win2, 10, 10, "System uptime:", COL_BLACK, COL_BGWIN);

    unsigned int ms = (unsigned int)uptime();
    unsigned int secs = ms / 1000;
    char buf[32];
    myutoa(secs, buf);
    mystrcat(buf, "s");

    win_fill_rect(win2, 10, 30, 180, FONT_H + 4, COL_BGWIN);
    win_draw_text(win2, 10, 32, buf, COL_BLUE, COL_BGWIN);

    win_flip(win2);
}

static void draw_win3(void)
{
    win_fill_rect(win3, 0, 0, 300, 120, COL_BGWIN);

    win_draw_text(win3, 10, 14, "Type something:", COL_BLACK, COL_BGWIN);

    textfield_draw(win3, &tfield);
    button_draw(win3, &btn_submit);

    /* Show last submitted value */
    if (submitted[0]) {
        char msg[TEXTFIELD_MAX + 12];
        mystrcpy(msg, "Got: ");
        mystrcat(msg, submitted);
        win_fill_rect(win3, 10, 106, 280, FONT_H, COL_BGWIN);
        win_draw_text(win3, 10, 106, msg, COL_DKGREY, COL_BGWIN);
    }

    win_flip(win3);
    btn_submit.pressed = 0;
}

/* ── event handling ──────────────────────────────────────────────────────── */

static void handle_win1_event(struct gui_event *ev)
{
    if (ev->type == GUI_EVENT_RESIZE) {
        draw_win1();
    } else if (ev->type == GUI_EVENT_MOUSE_BTN &&
        (ev->buttons & MOUSE_BTN_LEFT)) {
        if (button_hit(&btn_hello, ev->x, ev->y)) {
            btn_hello.pressed = 1;
            mystrcpy(label_text, "Hello, Luminary!");
        } else if (button_hit(&btn_clear, ev->x, ev->y)) {
            btn_clear.pressed = 1;
            mystrcpy(label_text, "Cleared.");
        }
        draw_win1();
    }
}

static void handle_win2_event(struct gui_event *ev)
{
    if (ev->type == GUI_EVENT_RESIZE)
        draw_win2();
}

static void handle_win3_event(struct gui_event *ev)
{
    if (ev->type == GUI_EVENT_RESIZE) {
        draw_win3();
    } else if (ev->type == GUI_EVENT_MOUSE_BTN &&
        (ev->buttons & MOUSE_BTN_LEFT)) {
        if (button_hit(&btn_submit, ev->x, ev->y)) {
            btn_submit.pressed = 1;
            mystrcpy(submitted, tfield.buf);
            /* Clear field */
            tfield.buf[0] = '\0';
            tfield.len    = 0;
        }
        draw_win3();
    } else if (ev->type == GUI_EVENT_KEYPRESS) {
        textfield_key(&tfield, ev->key);
        draw_win3();
    }
}

/* ── main loop ───────────────────────────────────────────────────────────── */

static unsigned int last_uptime_draw = 0;

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    init_colours();
    tfield.focused = 1;

    /* Create windows */
    win1 = win_create(100, 80,  320, 120 + 20 + 2*2, "Demo: Buttons");
    win2 = win_create(460, 80,  220, 80  + 20 + 2*2, "Uptime");
    win3 = win_create(100, 260, 320, 120 + 20 + 2*2, "Demo: Text Input");
    win4 = win_create(700, 80,  380, 300 + 20 + 2*2, "Console");

    /* Initial draw */
    draw_win1();
    draw_win2();
    draw_win3();
    con_push_line("Luminary console. Type 'help'.");
    draw_win4();

    /* Event loop */
    for (;;) {
        struct gui_event ev;
        int any = 0;

        if (win1 >= 0 && win_poll_event(win1, &ev)) {
            if (ev.type == GUI_EVENT_CLOSE) { win1 = -1; }
            else { handle_win1_event(&ev); any = 1; }
        }

        if (win2 >= 0 && win_poll_event(win2, &ev)) {
            if (ev.type == GUI_EVENT_CLOSE) { win2 = -1; }
            else { handle_win2_event(&ev); any = 1; }
        }

        if (win3 >= 0 && win_poll_event(win3, &ev)) {
            if (ev.type == GUI_EVENT_CLOSE) { win3 = -1; }
            else { handle_win3_event(&ev); any = 1; }
        }

        if (win4 >= 0 && win_poll_event(win4, &ev)) {
            if (ev.type == GUI_EVENT_CLOSE) { win4 = -1; }
            else { handle_win4_event(&ev); any = 1; }
        }

        /* Exit when all windows have been closed */
        if (win1 < 0 && win2 < 0 && win3 < 0 && win4 < 0)
            exit(0);

        /* Refresh all windows every 1s (also catches post-resize redraws) */
        unsigned int now = (unsigned int)uptime();
        if (now - last_uptime_draw >= 1000) {
            last_uptime_draw = now;
            if (win1 >= 0) draw_win1();
            if (win2 >= 0) draw_win2();
            if (win3 >= 0) draw_win3();
            if (win4 >= 0) draw_win4();
            any = 1;
        }

        /* Yield CPU when idle to avoid spinning and let compositor run */
        if (!any)
            yield();
    }
}
