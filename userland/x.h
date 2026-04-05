/* x.h — userland /dev/x ioctl API.
 *
 * Open /dev/x, then use ioctl() for all window and desktop operations.
 * Mouse state is read with read(fd, &struct x_mouse_state, sizeof(...)).
 *
 * All colours are 0xFFRRGGBB.
 */
#pragma once

#include "syscall.h"

/* ── window chrome dimensions (must match kernel/gui.h) ─────────────────── */
#define GUI_TITLE_HEIGHT    20
#define GUI_BORDER          2
#define GUI_CHROME_W        (GUI_BORDER * 2)
#define GUI_CHROME_H        (GUI_TITLE_HEIGHT + GUI_BORDER * 2)

/* ── event types ─────────────────────────────────────────────────────────── */
#define GUI_EVENT_NONE      0
#define GUI_EVENT_KEYPRESS  1
#define GUI_EVENT_MOUSE_BTN 2
#define GUI_EVENT_RESIZE    3
#define GUI_EVENT_CLOSE     4

/* ── mouse button flags ──────────────────────────────────────────────────── */
#define MOUSE_BTN_LEFT   1
#define MOUSE_BTN_RIGHT  2
#define MOUSE_BTN_MIDDLE 4

/* ── ioctl request codes ─────────────────────────────────────────────────── */
#define X_WIN_CREATE        1
#define X_WIN_DESTROY       2
#define X_WIN_FILL_RECT     3
#define X_WIN_DRAW_RECT     4
#define X_WIN_DRAW_TEXT     5
#define X_WIN_FLIP          6
#define X_WIN_POLL_EVENT    7
#define X_WIN_GET_SIZE      8
#define X_SET_BG            9
#define X_SET_DESKTOP_COLOR 10

/* ── event struct (must match kernel/gui.h layout) ───────────────────────── */
struct gui_event {
    unsigned char  type;
    char           key;
    unsigned short x, y;
    unsigned char  buttons;
};

/* ── ioctl argument structs ──────────────────────────────────────────────── */

struct x_win_create {
    int          x, y;
    unsigned int w, h;
    const char  *title;
};

struct x_win_destroy {
    int id;
};

struct x_win_rect {
    int          id;
    unsigned int x, y, w, h;
    unsigned int color;
};

struct x_win_text {
    int          id;
    unsigned int x, y;
    const char  *str;
    unsigned int fgcolor, bgcolor;
};

struct x_win_flip {
    int id;
};

struct x_win_poll_event {
    int               id;
    struct gui_event *ev;
};

struct x_win_get_size {
    int          id;
    unsigned int w, h;  /* filled on return */
};

struct x_set_bg {
    const unsigned int *pixels;
    unsigned int        w, h;
};

struct x_set_desktop_color {
    unsigned int r, g, b;
};

/* ── mouse state (returned by read on /dev/x) ────────────────────────────── */
struct x_mouse_state {
    unsigned int x, y, buttons;
};

/* ── colour helper ───────────────────────────────────────────────────────── */
static inline unsigned int rgb(unsigned char r, unsigned char g, unsigned char b)
{
    return 0xFF000000u | ((unsigned int)r << 16) | ((unsigned int)g << 8) | b;
}

/* ── high-level wrappers ─────────────────────────────────────────────────── */

static inline int win_create(int xfd, int x, int y, unsigned int w, unsigned int h,
                              const char *title)
{
    struct x_win_create req = { x, y, w, h, title };
    return ioctl(xfd, X_WIN_CREATE, &req);
}

static inline void win_destroy(int xfd, int id)
{
    struct x_win_destroy req = { id };
    ioctl(xfd, X_WIN_DESTROY, &req);
}

static inline void win_fill_rect(int xfd, int id,
                                  unsigned int x, unsigned int y,
                                  unsigned int w, unsigned int h,
                                  unsigned int color)
{
    struct x_win_rect req = { id, x, y, w, h, color };
    ioctl(xfd, X_WIN_FILL_RECT, &req);
}

static inline void win_draw_rect(int xfd, int id,
                                  unsigned int x, unsigned int y,
                                  unsigned int w, unsigned int h,
                                  unsigned int color)
{
    struct x_win_rect req = { id, x, y, w, h, color };
    ioctl(xfd, X_WIN_DRAW_RECT, &req);
}

static inline void win_draw_text(int xfd, int id,
                                  unsigned int x, unsigned int y,
                                  const char *str,
                                  unsigned int fgcolor, unsigned int bgcolor)
{
    struct x_win_text req = { id, x, y, str, fgcolor, bgcolor };
    ioctl(xfd, X_WIN_DRAW_TEXT, &req);
}

static inline void win_flip(int xfd, int id)
{
    struct x_win_flip req = { id };
    ioctl(xfd, X_WIN_FLIP, &req);
}

static inline int win_poll_event(int xfd, int id, struct gui_event *ev)
{
    struct x_win_poll_event req = { id, ev };
    return ioctl(xfd, X_WIN_POLL_EVENT, &req);
}

static inline int win_get_size(int xfd, int id, unsigned int *cw, unsigned int *ch)
{
    struct x_win_get_size req = { id, 0, 0 };
    int r = ioctl(xfd, X_WIN_GET_SIZE, &req);
    if (r == 0) { *cw = req.w; *ch = req.h; }
    return r;
}

static inline void mouse_get(int xfd, struct x_mouse_state *ms)
{
    read(xfd, (char *)ms, sizeof(*ms));
}

static inline int x_set_bg(int xfd, const unsigned int *pixels,
                             unsigned int w, unsigned int h)
{
    struct x_set_bg req = { pixels, w, h };
    return ioctl(xfd, X_SET_BG, &req);
}

static inline void x_set_desktop_color(int xfd, unsigned int r,
                                         unsigned int g, unsigned int b)
{
    struct x_set_desktop_color req = { r, g, b };
    ioctl(xfd, X_SET_DESKTOP_COLOR, &req);
}
