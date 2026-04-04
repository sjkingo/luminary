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
#include "libc/stdio.h"
#include "libc/string.h"

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

#define FONT_W  8
#define FONT_H  16

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
    unsigned int tw = strlen(btn->label) * FONT_W;
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
    strcpy(display, tf->buf);
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

static void init_colours(void)
{
    btn_hello.col_normal  = COL_BLUE;
    btn_hello.col_hover   = COL_LBLUE;
    btn_clear.col_normal  = COL_RED;
    btn_clear.col_hover   = COL_LRED;
    btn_submit.col_normal = COL_GREEN;
    btn_submit.col_hover  = COL_LGREEN;
}
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
    snprintf(buf, sizeof(buf), "%us", secs);

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
        snprintf(msg, sizeof(msg), "Got: %s", submitted);
        win_fill_rect(win3, 10, 106, 280, FONT_H, COL_BGWIN);
        win_draw_text(win3, 10, 106, msg, COL_DKGREY, COL_BGWIN);
    }

    win_flip(win3);
    btn_submit.pressed = 0;
}
static void handle_win1_event(struct gui_event *ev)
{
    if (ev->type == GUI_EVENT_RESIZE) {
        draw_win1();
    } else if (ev->type == GUI_EVENT_MOUSE_BTN &&
        (ev->buttons & MOUSE_BTN_LEFT)) {
        if (button_hit(&btn_hello, ev->x, ev->y)) {
            btn_hello.pressed = 1;
            strcpy(label_text, "Hello, Luminary!");
        } else if (button_hit(&btn_clear, ev->x, ev->y)) {
            btn_clear.pressed = 1;
            strcpy(label_text, "Cleared.");
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
            strcpy(submitted, tfield.buf);
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
static unsigned int last_uptime_draw = 0;

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    init_colours();
    tfield.focused = 1;

    /* Create windows */
    win1 = win_create(100, 80,  320 + GUI_CHROME_W, 120 + GUI_CHROME_H, "Demo: Buttons");
    win2 = win_create(460, 80,  220 + GUI_CHROME_W, 80  + GUI_CHROME_H, "Uptime");
    win3 = win_create(100, 260, 320 + GUI_CHROME_W, 120 + GUI_CHROME_H, "Demo: Text Input");

    /* Initial draw */
    draw_win1();
    draw_win2();
    draw_win3();

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

        /* Exit when all windows have been closed */
        if (win1 < 0 && win2 < 0 && win3 < 0)
            exit(0);

        /* Refresh all windows every 1s (also catches post-resize redraws) */
        unsigned int now = (unsigned int)uptime();
        if (now - last_uptime_draw >= 1000) {
            last_uptime_draw = now;
            if (win1 >= 0) draw_win1();
            if (win2 >= 0) draw_win2();
            if (win3 >= 0) draw_win3();
            any = 1;
        }

        /* Yield CPU when idle to avoid spinning and let compositor run */
        if (!any)
            yield();
    }
}
