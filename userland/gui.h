/* Luminary GUI userspace library.
 *
 * Syscall convention: EAX=num, EBX=arg0, ECX=arg1, EDX=arg2.
 * Extra args for GUI calls are pushed onto the user stack before int $0x80
 * so the kernel can read them from uesp+4, uesp+8, uesp+12.
 *
 * All colours are 0xFFRRGGBB.
 */

#pragma once

/* ── syscall numbers ─────────────────────────────────────────────────────── */
#define SYS_WIN_CREATE      8
#define SYS_WIN_DESTROY     9
#define SYS_WIN_FILL_RECT   10
#define SYS_WIN_DRAW_TEXT   11
#define SYS_WIN_FLIP        12
#define SYS_WIN_POLL_EVENT  13
#define SYS_MOUSE_GET       14
#define SYS_WIN_DRAW_RECT   15
#define SYS_WIN_GET_SIZE    19
#define SYS_READ_NB         20

/* ── event types ─────────────────────────────────────────────────────────── */
#define GUI_EVENT_NONE      0
#define GUI_EVENT_KEYPRESS  1
#define GUI_EVENT_MOUSE_BTN 2
#define GUI_EVENT_RESIZE    3   /* x=new client w, y=new client h */
#define GUI_EVENT_CLOSE     4   /* window was destroyed by the compositor */

/* ── mouse button flags ──────────────────────────────────────────────────── */
#define MOUSE_BTN_LEFT   1
#define MOUSE_BTN_RIGHT  2
#define MOUSE_BTN_MIDDLE 4

/* ── structs (must match kernel/gui.h layout) ────────────────────────────── */

struct gui_event {
    unsigned char  type;
    char           key;
    unsigned short x, y;
    unsigned char  buttons;
};

struct mouse_state {
    unsigned int x, y, buttons;
};

/* ── colour helper ───────────────────────────────────────────────────────── */
static inline unsigned int rgb(unsigned char r, unsigned char g, unsigned char b)
{
    return 0xFF000000u | ((unsigned int)r << 16) | ((unsigned int)g << 8) | b;
}

/* ── low-level syscall wrappers ──────────────────────────────────────────── */

static inline int _sc3(int n, unsigned int a, unsigned int b, unsigned int c)
{
    int ret;
    __asm__ volatile("int $0x80"
        : "=a"(ret) : "a"(n), "b"(a), "c"(b), "d"(c) : "memory");
    return ret;
}

/* Push three extra words onto the stack, then invoke int $0x80.
 * The kernel reads them from frame->uesp + 4/8/12.
 * We store the extra args as locals and use "m" constraints so no extra
 * registers are needed beyond eax/ebx/ecx/edx. */
static inline int _sc3x3(int n,
                          unsigned int a, unsigned int b, unsigned int c,
                          unsigned int x, unsigned int y, unsigned int z)
{
    int ret;
    volatile unsigned int _x = x, _y = y, _z = z;
    __asm__ volatile(
        "pushl %[z]\n\t"
        "pushl %[y]\n\t"
        "pushl %[x]\n\t"
        "int   $0x80\n\t"
        "addl  $12, %%esp\n\t"
        : "=a"(ret)
        : "a"(n), "b"(a), "c"(b), "d"(c),
          [x]"m"(_x), [y]"m"(_y), [z]"m"(_z)
        : "memory");
    return ret;
}

/* Push one extra word, then invoke int $0x80. */
static inline int _sc3x1(int n,
                          unsigned int a, unsigned int b, unsigned int c,
                          unsigned int x)
{
    int ret;
    volatile unsigned int _x = x;
    __asm__ volatile(
        "pushl %[x]\n\t"
        "int   $0x80\n\t"
        "addl  $4, %%esp\n\t"
        : "=a"(ret)
        : "a"(n), "b"(a), "c"(b), "d"(c),
          [x]"m"(_x)
        : "memory");
    return ret;
}

/* ── public API ──────────────────────────────────────────────────────────── */

/*
 * win_create(x, y, w, h, title) -> window id or -1
 * Kernel reads: EBX=x, ECX=y, EDX=w, [uesp+4]=h, [uesp+8]=title
 */
static inline int win_create(int x, int y, unsigned int w, unsigned int h,
                              const char *title)
{
    return _sc3x3(SYS_WIN_CREATE,
                  (unsigned int)x, (unsigned int)y, w,
                  h, (unsigned int)title, 0);
}

static inline void win_destroy(int id)
{
    _sc3(SYS_WIN_DESTROY, (unsigned int)id, 0, 0);
}

/*
 * win_fill_rect(id, x, y, w, h, color)
 * Kernel reads: EBX=id, ECX=x, EDX=y, [uesp+4]=w, [uesp+8]=h, [uesp+12]=color
 */
static inline void win_fill_rect(int id,
                                  unsigned int x, unsigned int y,
                                  unsigned int w, unsigned int h,
                                  unsigned int color)
{
    _sc3x3(SYS_WIN_FILL_RECT,
           (unsigned int)id, x, y,
           w, h, color);
}

static inline void win_draw_rect(int id,
                                  unsigned int x, unsigned int y,
                                  unsigned int w, unsigned int h,
                                  unsigned int color)
{
    _sc3x3(SYS_WIN_DRAW_RECT,
           (unsigned int)id, x, y,
           w, h, color);
}

/*
 * win_draw_text(id, x, y, str, fgcolor, bgcolor)
 * Kernel reads: EBX=id, ECX=x, EDX=y, [uesp+4]=str, [uesp+8]=fg, [uesp+12]=bg
 */
static inline void win_draw_text(int id,
                                  unsigned int x, unsigned int y,
                                  const char *str,
                                  unsigned int fgcolor, unsigned int bgcolor)
{
    _sc3x3(SYS_WIN_DRAW_TEXT,
           (unsigned int)id, x, y,
           (unsigned int)str, fgcolor, bgcolor);
}

/* Commit backbuffer to screen */
static inline void win_flip(int id)
{
    _sc3(SYS_WIN_FLIP, (unsigned int)id, 0, 0);
}

/* Non-blocking event poll. Returns 1 if ev filled, 0 if none. */
static inline int win_poll_event(int id, struct gui_event *ev)
{
    return _sc3(SYS_WIN_POLL_EVENT, (unsigned int)id, (unsigned int)ev, 0);
}

/* Get current mouse state (always succeeds) */
static inline void mouse_get(struct mouse_state *ms)
{
    _sc3(SYS_MOUSE_GET, (unsigned int)ms, 0, 0);
}

/* Get client area size of a window. Returns 0 on success. */
static inline int win_get_size(int id, unsigned int *cw, unsigned int *ch)
{
    return _sc3(SYS_WIN_GET_SIZE, (unsigned int)id,
                (unsigned int)cw, (unsigned int)ch);
}

/* Non-blocking keyboard read. Returns chars read (0 if none). */
static inline int read_nb(char *buf, unsigned int len)
{
    return _sc3(SYS_READ_NB, (unsigned int)buf, len, 0);
}
