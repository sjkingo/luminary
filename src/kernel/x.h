/* x.h — /dev/x ioctl request codes and argument structs.
 *
 * Shared between the kernel (gui.c) and userland (userland/x.h mirrors this).
 * All GUI operations previously handled as individual syscalls now go through
 * SYS_IOCTL on an open fd to /dev/x.
 *
 * Convention: ioctl(fd, X_*, &struct x_*) -> int32_t result.
 * Mouse state is read via read(fd, &struct x_mouse_state, sizeof(...)).
 */
#pragma once

#include <stdint.h>

/* ── request codes ───────────────────────────────────────────────────────── */
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

/* ── argument structs ────────────────────────────────────────────────────── */

struct x_win_create {
    int32_t     x, y;
    uint32_t    w, h;
    const char *title;
};

struct x_win_destroy {
    int32_t id;
};

struct x_win_rect {
    int32_t  id;
    uint32_t x, y, w, h;
    uint32_t color;
};

struct x_win_text {
    int32_t     id;
    uint32_t    x, y;
    const char *str;
    uint32_t    fgcolor, bgcolor;
};

struct x_win_flip {
    int32_t id;
};

struct x_win_poll_event {
    int32_t  id;
    void    *ev;    /* points to struct gui_event in caller's space */
};

struct x_win_get_size {
    int32_t  id;
    uint32_t w, h;  /* filled on return */
};

struct x_set_bg {
    const uint32_t *pixels;
    uint32_t        w, h;
};

struct x_set_desktop_color {
    uint32_t r, g, b;
};

/* ── mouse state (returned by read on /dev/x) ────────────────────────────── */
struct x_mouse_state {
    uint32_t x, y, buttons;
};
