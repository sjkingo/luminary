/* Luminary GUI userspace library.
 * Wraps kernel GUI syscalls with a convenient C API.
 * All drawing goes into per-window backbuffers; call gui_flip() to display.
 */

#pragma once

/* ── syscall numbers (must match kernel/syscall.h) ───────────────────────── */
#define SYS_WIN_CREATE      8
#define SYS_WIN_DESTROY     9
#define SYS_WIN_FILL_RECT   10
#define SYS_WIN_DRAW_TEXT   11
#define SYS_WIN_FLIP        12
#define SYS_WIN_POLL_EVENT  13
#define SYS_MOUSE_GET       14
#define SYS_WIN_DRAW_RECT   15

/* ── event types (must match kernel/gui.h) ───────────────────────────────── */
#define GUI_EVENT_NONE      0
#define GUI_EVENT_KEYPRESS  1
#define GUI_EVENT_MOUSE_BTN 2

/* ── mouse button flags ──────────────────────────────────────────────────── */
#define MOUSE_BTN_LEFT   1
#define MOUSE_BTN_RIGHT  2
#define MOUSE_BTN_MIDDLE 4

/* ── structs ─────────────────────────────────────────────────────────────── */

struct gui_event {
    unsigned char  type;
    char           key;
    unsigned short x, y;
    unsigned char  buttons;
};

struct mouse_state {
    unsigned int x, y;
    unsigned int buttons;
};

/* ── colour helper ───────────────────────────────────────────────────────── */
static inline unsigned int gui_rgb(unsigned char r, unsigned char g,
                                   unsigned char b)
{
    return 0xFF000000u | ((unsigned int)r << 16) | ((unsigned int)g << 8) | b;
}

/* ── syscall helpers ─────────────────────────────────────────────────────── */

/* 3-register syscall (EAX=num, EBX=a, ECX=b, EDX=c) */
static inline int _sc3(int num, unsigned int a, unsigned int b, unsigned int c)
{
    int ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(num), "b"(a), "c"(b), "d"(c)
                     : "memory");
    return ret;
}

/* Syscall with extra args pushed onto the stack before the int.
 * We push extras manually so the kernel can read them from uesp. */

static inline int _sc3_stack3(int num,
                               unsigned int a, unsigned int b, unsigned int c,
                               unsigned int x, unsigned int y, unsigned int z)
{
    int ret;
    __asm__ volatile(
        "push %7\n\t"
        "push %6\n\t"
        "push %5\n\t"
        "int $0x80\n\t"
        "add $12, %%esp\n\t"
        : "=a"(ret)
        : "a"(num), "b"(a), "c"(b), "d"(c),
          "r"(x), "r"(y), "r"(z)
        : "memory");
    return ret;
}

static inline int _sc3_stack1(int num,
                               unsigned int a, unsigned int b, unsigned int c,
                               unsigned int x)
{
    int ret;
    __asm__ volatile(
        "push %4\n\t"
        "int $0x80\n\t"
        "add $4, %%esp\n\t"
        : "=a"(ret)
        : "a"(num), "b"(a), "c"(b), "d"(c), "r"(x)
        : "memory");
    return ret;
}

/* ── public API ──────────────────────────────────────────────────────────── */

/* Create a window. Returns window ID or -1 on failure. */
static inline int gui_create(int x, int y, unsigned int w, unsigned int h,
                              const char *title)
{
    return _sc3_stack3(SYS_WIN_CREATE,
                       (unsigned int)x, (unsigned int)y, w,
                       h, (unsigned int)title, 0);
}

static inline void gui_destroy(int id)
{
    _sc3(SYS_WIN_DESTROY, (unsigned int)id, 0, 0);
}

static inline void gui_fill_rect(int id, unsigned int x, unsigned int y,
                                 unsigned int w, unsigned int h,
                                 unsigned int color)
{
    _sc3_stack3(SYS_WIN_FILL_RECT,
                (unsigned int)id, x, y,
                w, h, color);
}

static inline void gui_draw_rect(int id, unsigned int x, unsigned int y,
                                 unsigned int w, unsigned int h,
                                 unsigned int color)
{
    _sc3_stack3(SYS_WIN_DRAW_RECT,
                (unsigned int)id, x, y,
                w, h, color);
}

static inline void gui_draw_text(int id, unsigned int x, unsigned int y,
                                 const char *str,
                                 unsigned int fgcolor, unsigned int bgcolor)
{
    _sc3_stack3(SYS_WIN_DRAW_TEXT,
                (unsigned int)id, x, y,
                (unsigned int)str, fgcolor, bgcolor);
}

/* Commit backbuffer to screen via compositor */
static inline void gui_flip(int id)
{
    _sc3(SYS_WIN_FLIP, (unsigned int)id, 0, 0);
}

/* Non-blocking event poll. Returns 1 if event filled, 0 if none. */
static inline int gui_poll_event(int id, struct gui_event *ev)
{
    return _sc3(SYS_WIN_POLL_EVENT, (unsigned int)id, (unsigned int)ev, 0);
}

/* Get current mouse state */
static inline void gui_mouse_get(struct mouse_state *ms)
{
    _sc3(SYS_MOUSE_GET, (unsigned int)ms, 0, 0);
}
