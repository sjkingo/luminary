/* Compositor / window manager kernel task.
 *
 * Architecture:
 *   - Window list: singly-linked list of struct window, z-ordered (head = topmost)
 *   - Each window has a client-area backbuffer (w × client_h × 4 bytes) in kernel heap
 *   - Compositor task sleeps (suspends itself) when idle, woken by gui_wake()
 *   - On each wake: composite all windows to framebuffer, draw cursor
 *   - Mouse drag is handled entirely inside the compositor task
 *
 * Coordinate system: (0,0) is top-left of screen.
 * Window y,x = top-left of the title bar.
 * Client area starts at (x + BORDER, y + TITLE_HEIGHT).
 */

#include <string.h>
#include <stdbool.h>

#include "kernel/gui.h"
#include "kernel/kernel.h"
#include "kernel/heap.h"
#include "kernel/pmm.h"
#include "kernel/vmm.h"
#include "kernel/sched.h"
#include "kernel/task.h"
#include "kernel/syscall.h"
#include "drivers/fbdev.h"
#include "drivers/vbe.h"
#include "drivers/mouse.h"
#include "drivers/keyboard.h"
#include "cpu/x86.h"
#include "fonts/vga8x16.h"

/* ── framebuffer geometry (filled by init_gui from VBE) ─────────────────── */
static char    *fb;            /* draw target: points to back during compose, fb_hw otherwise */
static char    *fb_hw;         /* real hardware framebuffer address */
static char    *back;          /* full-screen back buffer (allocated at compositor start) */
static uint32_t fb_w;
static uint32_t fb_h;
static uint16_t fb_pitch;
static uint8_t  fb_depth;      /* bits per pixel (always 32 here) */
static uint32_t fb_bpp;        /* bytes per pixel */

/* ── forward declarations ────────────────────────────────────────────────── */
static void gui_start_compositor(void);

/* ── window list ─────────────────────────────────────────────────────────── */
static struct window  win_pool[GUI_MAX_WINDOWS];
static struct window *win_list;   /* head = topmost */
static int            next_id = 1;

/* ── compositor task pointer (so gui_wake can find it) ───────────────────── */
static struct task *compositor_task_ptr = NULL;

/* ── dirty flag: set when something needs redrawing ─────────────────────── */
static bool compositor_dirty = false;

/* ── set when the compositor should exit (all windows closed) ────────────── */
static bool compositor_quit = false;


/* ── previous cursor position (to detect movement) ──────────────────────── */
static uint32_t prev_cursor_x = 0xFFFFFFFF;
static uint32_t prev_cursor_y = 0xFFFFFFFF;

/* ── drag state ──────────────────────────────────────────────────────────── */
static struct window *drag_win    = NULL;
static int32_t        drag_off_x  = 0;
static int32_t        drag_off_y  = 0;

/* ── previous mouse button state (for edge detection) ───────────────────── */
static uint8_t prev_buttons = 0;

/* ── cursor sprite ───────────────────────────────────────────────────────── */
#define CURSOR_W 12
#define CURSOR_H 19
static const uint8_t cursor_mask[CURSOR_H][CURSOR_W] = {
    {1,0,0,0,0,0,0,0,0,0,0,0},
    {1,1,0,0,0,0,0,0,0,0,0,0},
    {1,1,1,0,0,0,0,0,0,0,0,0},
    {1,1,1,1,0,0,0,0,0,0,0,0},
    {1,1,1,1,1,0,0,0,0,0,0,0},
    {1,1,1,1,1,1,0,0,0,0,0,0},
    {1,1,1,1,1,1,1,0,0,0,0,0},
    {1,1,1,1,1,1,1,1,0,0,0,0},
    {1,1,1,1,1,1,1,1,1,0,0,0},
    {1,1,1,1,1,1,0,0,0,0,0,0},
    {1,1,1,0,1,1,1,0,0,0,0,0},
    {1,1,0,0,0,1,1,1,0,0,0,0},
    {1,0,0,0,0,0,1,1,1,0,0,0},
    {0,0,0,0,0,0,0,1,1,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0},
};

/* Bounding box including 1px outline on all sides */

/* ── colour palette ──────────────────────────────────────────────────────── */
#define COL_DESKTOP     rgb(30,  30,  50)
#define COL_TITLEBAR    rgb(60,  90, 160)
#define COL_TITLEBAR_UF rgb(80,  80,  80)   /* unfocused */
#define COL_TITLE_TEXT  rgb(255,255,255)
#define COL_BORDER      rgb(40,  60, 120)
#define COL_CLOSE_BTN   rgb(200,  60,  60)
#define COL_CLIENT_BG   rgb(240, 240, 240)
#define COL_CURSOR      rgb(255,255,255)
#define COL_CURSOR_OUT  rgb(0,   0,   0)

/* ═══════════════════════════════════════════════════════════════════════════
 * Low-level pixel drawing (direct to framebuffer)
 * ═══════════════════════════════════════════════════════════════════════════ */

static inline void fb_putpixel(int32_t x, int32_t y, uint32_t color)
{
    if (x < 0 || y < 0 || (uint32_t)x >= fb_w || (uint32_t)y >= fb_h)
        return;
    *(uint32_t *)(fb + y * fb_pitch + x * fb_bpp) = color;
}

static void fb_fill_rect(int32_t x, int32_t y, uint32_t w, uint32_t h,
                         uint32_t color)
{
    /* Clip against left/top edges */
    int32_t x0 = x, y0 = y;
    int32_t x1 = x + (int32_t)w;
    int32_t y1 = y + (int32_t)h;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (int32_t)fb_w) x1 = (int32_t)fb_w;
    if (y1 > (int32_t)fb_h) y1 = (int32_t)fb_h;
    if (x0 >= x1 || y0 >= y1) return;
    for (int32_t row = y0; row < y1; row++) {
        uint32_t *p = (uint32_t *)(fb + row * fb_pitch + x0 * fb_bpp);
        for (int32_t col = x0; col < x1; col++)
            *p++ = color;
    }
}

static void fb_draw_rect_outline(int32_t x, int32_t y, uint32_t w, uint32_t h,
                                 uint32_t color)
{
    fb_fill_rect(x,                   y,                   w, 1, color); /* top */
    fb_fill_rect(x,                   y + (int32_t)h - 1,  w, 1, color); /* bottom */
    fb_fill_rect(x,                   y,                   1, h, color); /* left */
    fb_fill_rect(x + (int32_t)w - 1,  y,                   1, h, color); /* right */
}

static void fb_draw_char(uint8_t c, int32_t x, int32_t y,
                         uint32_t fgcolor, uint32_t bgcolor, bool transparent_bg)
{
    uint32_t glyph_off = (uint32_t)c * CONSOLE_FONT_WIDTH * CONSOLE_FONT_HEIGHT / 8;
    uint32_t b = 0;
    for (uint32_t j = 0; j < CONSOLE_FONT_HEIGHT; j++) {
        int32_t py = y + (int32_t)j;
        if (py < 0) { b += CONSOLE_FONT_WIDTH; continue; }
        if ((uint32_t)py >= fb_h) break;
        for (uint32_t i = 0; i < CONSOLE_FONT_WIDTH; i++) {
            bool set = (fontdata[glyph_off + b / 8] >> (7 - (b % 8))) & 1;
            int32_t px = x + (int32_t)i;
            if (px >= 0 && (uint32_t)px < fb_w) {
                uint32_t *p = (uint32_t *)(fb + py * fb_pitch + px * fb_bpp);
                if (set)
                    *p = fgcolor;
                else if (!transparent_bg)
                    *p = bgcolor;
            }
            b++;
        }
    }
}

static void fb_draw_text(const char *str, int32_t x, int32_t y,
                         uint32_t fgcolor, uint32_t bgcolor, bool transparent_bg)
{
    while (*str) {
        fb_draw_char((uint8_t)*str++, x, y, fgcolor, bgcolor, transparent_bg);
        x += CONSOLE_FONT_WIDTH;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Window list helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

static struct window *find_window(int id)
{
    struct window *w = win_list;
    while (w) {
        if (w->id == id) return w;
        w = w->next;
    }
    return NULL;
}

/* Remove window from list (does not free resources) */
static void win_list_remove(struct window *target)
{
    if (win_list == target) {
        win_list = target->next;
        return;
    }
    struct window *w = win_list;
    while (w && w->next != target)
        w = w->next;
    if (w)
        w->next = target->next;
}

/* Insert at front (topmost) */
static void win_list_push_front(struct window *w)
{
    w->next = win_list;
    win_list = w;
}

/* Client area height */
static inline uint32_t client_h(struct window *w)
{
    return w->h > GUI_TITLE_HEIGHT + GUI_BORDER * 2
           ? w->h - GUI_TITLE_HEIGHT - GUI_BORDER * 2
           : 0;
}

/* Client area width */
static inline uint32_t client_w(struct window *w)
{
    return w->w > GUI_BORDER * 2
           ? w->w - GUI_BORDER * 2
           : 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Window backbuffer drawing (into per-window buffer, not framebuffer)
 * ═══════════════════════════════════════════════════════════════════════════ */

static void bb_fill_rect(struct window *w, uint32_t x, uint32_t y,
                         uint32_t rw, uint32_t rh, uint32_t color)
{
    if (!w->backbuffer) return;
    uint32_t cw = client_w(w);
    uint32_t ch = client_h(w);
    for (uint32_t row = y; row < y + rh && row < ch; row++) {
        for (uint32_t col = x; col < x + rw && col < cw; col++) {
            w->backbuffer[row * cw + col] = color;
        }
    }
}

static void bb_draw_rect_outline(struct window *w, uint32_t x, uint32_t y,
                                 uint32_t rw, uint32_t rh, uint32_t color)
{
    bb_fill_rect(w, x,        y,        rw, 1,  color);
    bb_fill_rect(w, x,        y + rh-1, rw, 1,  color);
    bb_fill_rect(w, x,        y,        1,  rh, color);
    bb_fill_rect(w, x + rw-1, y,        1,  rh, color);
}

static void bb_draw_text(struct window *w, uint32_t x, uint32_t y,
                         const char *str, uint32_t fgcolor, uint32_t bgcolor)
{
    if (!w->backbuffer) return;
    uint32_t cw = client_w(w);
    uint32_t ch = client_h(w);

    while (*str) {
        uint8_t c = (uint8_t)*str++;
        uint32_t glyph_off = (uint32_t)c * CONSOLE_FONT_WIDTH * CONSOLE_FONT_HEIGHT / 8;
        uint32_t b = 0;
        for (uint32_t j = 0; j < CONSOLE_FONT_HEIGHT; j++) {
            for (uint32_t i = 0; i < CONSOLE_FONT_WIDTH; i++) {
                bool set = (fontdata[glyph_off + b / 8] >> (7 - (b % 8))) & 1;
                uint32_t px = x + i, py = y + j;
                if (px < cw && py < ch)
                    w->backbuffer[py * cw + px] = set ? fgcolor : bgcolor;
                b++;
            }
        }
        x += CONSOLE_FONT_WIDTH;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Compositing
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Draw the desktop background */
static void draw_desktop(void)
{
    fb_fill_rect(0, 0, fb_w, fb_h, COL_DESKTOP);
}

/* Draw one window chrome + blit its backbuffer to the framebuffer */
static void draw_window(struct window *w)
{
    int32_t wx = w->x, wy = w->y;
    uint32_t ww = w->w, wh = w->h;

    /* Border */
    fb_fill_rect(wx, wy, ww, wh, COL_BORDER);

    /* Title bar */
    uint32_t tb_color = w->focused ? COL_TITLEBAR : COL_TITLEBAR_UF;
    fb_fill_rect(wx + GUI_BORDER,
                 wy + GUI_BORDER,
                 ww - GUI_BORDER * 2,
                 GUI_TITLE_HEIGHT,
                 tb_color);

    /* Title text (centred vertically in title bar) */
    int32_t ty = wy + GUI_BORDER + (GUI_TITLE_HEIGHT - CONSOLE_FONT_HEIGHT) / 2;
    fb_draw_text(w->title,
                 wx + GUI_BORDER + 4,
                 ty,
                 COL_TITLE_TEXT,
                 tb_color,
                 false);

    /* Close button (top-right of title bar) */
    int32_t cb_x = wx + (int32_t)ww - GUI_BORDER - GUI_CLOSE_BTN_SIZE - 2;
    int32_t cb_y = wy + GUI_BORDER + (GUI_TITLE_HEIGHT - GUI_CLOSE_BTN_SIZE) / 2;
    fb_fill_rect(cb_x, cb_y, GUI_CLOSE_BTN_SIZE, GUI_CLOSE_BTN_SIZE, COL_CLOSE_BTN);
    fb_draw_text("x", cb_x + 3, cb_y + 2, COL_TITLE_TEXT, COL_CLOSE_BTN, false);

    /* Client area background */
    int32_t ca_x = wx + GUI_BORDER;
    int32_t ca_y = wy + GUI_BORDER + GUI_TITLE_HEIGHT;
    uint32_t cw   = client_w(w);
    uint32_t ch   = client_h(w);

    if (w->backbuffer && cw > 0 && ch > 0) {
        /* Clip client area against screen bounds */
        int32_t src_x0 = 0;   /* first column of backbuffer to copy */
        int32_t dst_x  = ca_x;
        if (dst_x < 0) { src_x0 = -dst_x; dst_x = 0; }
        int32_t copy_w = (int32_t)cw - src_x0;
        if (dst_x + copy_w > (int32_t)fb_w) copy_w = (int32_t)fb_w - dst_x;
        if (copy_w <= 0) goto skip_blit;

        for (uint32_t row = 0; row < ch; row++) {
            int32_t dst_y = ca_y + (int32_t)row;
            if (dst_y < 0) continue;
            if ((uint32_t)dst_y >= fb_h) break;
            void *dst = fb + dst_y * fb_pitch + dst_x * fb_bpp;
            void *src = &w->backbuffer[row * cw + (uint32_t)src_x0];
            memcpy(dst, src, (uint32_t)copy_w * fb_bpp);
        }
        skip_blit:;
    } else {
        fb_fill_rect(ca_x, ca_y, cw, ch, COL_CLIENT_BG);
    }
}

static void draw_cursor(void)
{
    int32_t cx = (int32_t)mouse_x;
    int32_t cy = (int32_t)mouse_y;

    /* Outline pass (black) */
    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            if (dx == 0 && dy == 0) continue;
            for (uint32_t row = 0; row < CURSOR_H; row++) {
                for (uint32_t col = 0; col < CURSOR_W; col++) {
                    if (cursor_mask[row][col])
                        fb_putpixel(cx + (int32_t)col + dx,
                                    cy + (int32_t)row + dy,
                                    COL_CURSOR_OUT);
                }
            }
        }
    }
    /* Fill pass (white) */
    for (uint32_t row = 0; row < CURSOR_H; row++) {
        for (uint32_t col = 0; col < CURSOR_W; col++) {
            if (cursor_mask[row][col])
                fb_putpixel(cx + (int32_t)col, cy + (int32_t)row, COL_CURSOR);
        }
    }
}

/* Blit a horizontal span from back buffer to hardware framebuffer */
static inline void blit_rows(uint32_t y_start, uint32_t y_end)
{
    if (!back || !fb_hw || back == fb_hw) return;
    if (y_end > fb_h) y_end = fb_h;
    if (y_start >= y_end) return;
    memcpy(fb_hw + y_start * fb_pitch,
           back   + y_start * fb_pitch,
           (y_end - y_start) * fb_pitch);
}

/* scene_dirty: set when windows/desktop need full redraw (not just cursor) */
static bool scene_dirty = true;

/* Full composite: desktop → windows (back to front) → cursor → blit */
static void composite(void)
{
    if (scene_dirty) {
        /* Full redraw into back buffer */
        draw_desktop();

        struct window *stack[GUI_MAX_WINDOWS];
        int depth = 0;
        struct window *w = win_list;
        while (w && depth < GUI_MAX_WINDOWS) {
            if (w->visible) stack[depth++] = w;
            w = w->next;
        }
        for (int i = depth - 1; i >= 0; i--)
            draw_window(stack[i]);

        scene_dirty = false;

        /* Draw cursor into back buffer and blit everything */
        draw_cursor();
        if (back && fb_hw && back != fb_hw)
            memcpy(fb_hw, back, (uint32_t)fb_h * fb_pitch);
    } else {
        /* Cursor-only update: restore old cursor area from back buffer,
         * draw cursor at new position, blit only affected rows. */
        uint32_t old_y = prev_cursor_y;
        uint32_t new_y = mouse_y;

        /* Restore old cursor rows in back buffer from a clean back buffer
         * — but back buffer already has cursor drawn into it. We need the
         * scene without cursor. Simplest: just do a full redraw of those rows. */

        /* Clamp row ranges for old and new cursor positions */
        uint32_t r0 = (old_y >= 1 ? old_y - 1 : 0);
        uint32_t r1 = old_y + CURSOR_H + 1;
        uint32_t r2 = (new_y >= 1 ? new_y - 1 : 0);
        uint32_t r3 = new_y + CURSOR_H + 1;

        /* Repaint the union of old+new cursor rows in back buffer */
        uint32_t top    = r0 < r2 ? r0 : r2;
        uint32_t bottom = r1 > r3 ? r1 : r3;
        if (bottom > fb_h) bottom = fb_h;

        /* Repaint desktop + windows for those rows only */
        fb_fill_rect(0, top, fb_w, bottom - top, COL_DESKTOP);
        struct window *stack[GUI_MAX_WINDOWS];
        int depth = 0;
        struct window *w = win_list;
        while (w && depth < GUI_MAX_WINDOWS) {
            if (w->visible) stack[depth++] = w;
            w = w->next;
        }
        /* For each window that overlaps the dirty rows, redraw it clipped */
        for (int i = depth - 1; i >= 0; i--) {
            struct window *ww = stack[i];
            int32_t wy = ww->y;
            int32_t wh = (int32_t)ww->h;
            if (wy + wh < (int32_t)top || wy > (int32_t)bottom)
                continue;
            draw_window(ww);
        }

        draw_cursor();
        blit_rows(top, bottom);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Hit testing
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Returns the topmost window under (px, py), or NULL */
static struct window *hit_test(uint32_t px, uint32_t py)
{
    struct window *w = win_list;
    while (w) {
        if (w->visible &&
            (int32_t)px >= w->x &&
            (int32_t)py >= w->y &&
            (int32_t)px <  w->x + (int32_t)w->w &&
            (int32_t)py <  w->y + (int32_t)w->h)
            return w;
        w = w->next;
    }
    return NULL;
}

/* True if (px, py) is within the title bar of w */
static bool in_title_bar(struct window *w, uint32_t px, uint32_t py)
{
    return (int32_t)px >= w->x + GUI_BORDER &&
           (int32_t)px <  w->x + (int32_t)w->w - GUI_BORDER &&
           (int32_t)py >= w->y + GUI_BORDER &&
           (int32_t)py <  w->y + GUI_BORDER + GUI_TITLE_HEIGHT;
}

/* True if (px, py) is on the close button of w */
static bool in_close_btn(struct window *w, uint32_t px, uint32_t py)
{
    int32_t cb_x = w->x + (int32_t)w->w - GUI_BORDER - GUI_CLOSE_BTN_SIZE - 2;
    int32_t cb_y = w->y + GUI_BORDER + (GUI_TITLE_HEIGHT - GUI_CLOSE_BTN_SIZE) / 2;
    return (int32_t)px >= cb_x &&
           (int32_t)px <  cb_x + GUI_CLOSE_BTN_SIZE &&
           (int32_t)py >= cb_y &&
           (int32_t)py <  cb_y + GUI_CLOSE_BTN_SIZE;
}

/* Focus a window (move to front of list) */
static void focus_window(struct window *w)
{
    if (win_list == w) {
        w->focused = true;
        return;
    }
    /* Unfocus current top */
    if (win_list) win_list->focused = false;
    win_list_remove(w);
    win_list_push_front(w);
    w->focused = true;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Mouse event processing (called inside compositor loop)
 * ═══════════════════════════════════════════════════════════════════════════ */

static void process_mouse(void)
{
    struct mouse_event ev;

    while (mouse_read(&ev)) {
        compositor_dirty = true;
        uint32_t px = mouse_x;
        uint32_t py = mouse_y;

        uint8_t pressed  = ev.buttons & ~prev_buttons;  /* newly pressed */
        uint8_t released = ~ev.buttons & prev_buttons;  /* newly released */
        prev_buttons = ev.buttons;

        /* Drag: update position */
        if (drag_win && (ev.buttons & MOUSE_BTN_LEFT)) {
            drag_win->x = (int32_t)px - drag_off_x;
            drag_win->y = (int32_t)py - drag_off_y;
            scene_dirty = true;
        }

        /* Left button pressed */
        if (pressed & MOUSE_BTN_LEFT) {
            struct window *hit = hit_test(px, py);
            if (hit) {
                focus_window(hit);

                if (in_close_btn(hit, px, py)) {
                    gui_window_destroy(hit->id);
                    drag_win = NULL;
                } else if (in_title_bar(hit, px, py)) {
                    /* Begin drag */
                    drag_win   = hit;
                    drag_off_x = (int32_t)px - hit->x;
                    drag_off_y = (int32_t)py - hit->y;
                } else {
                    /* Click in client area: send event to window */
                    int32_t ca_x = hit->x + GUI_BORDER;
                    int32_t ca_y = hit->y + GUI_BORDER + GUI_TITLE_HEIGHT;
                    struct gui_event gev;
                    gev.type    = GUI_EVENT_MOUSE_BTN;
                    gev.x       = (uint16_t)((int32_t)px - ca_x);
                    gev.y       = (uint16_t)((int32_t)py - ca_y);
                    gev.buttons = ev.buttons;
                    gev.key     = 0;
                    gui_window_push_event(hit, &gev);
                }
            }
        }

        /* Left button released: end drag */
        if (released & MOUSE_BTN_LEFT) {
            drag_win = NULL;
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Keyboard routing (to focused window)
 * ═══════════════════════════════════════════════════════════════════════════ */

static void process_keyboard(void)
{
    if (!win_list || !win_list->focused) return;

    char c;
    while (keyboard_read(&c, 1) == 1) {
        struct gui_event ev;
        ev.type    = GUI_EVENT_KEYPRESS;
        ev.key     = c;
        ev.x       = 0;
        ev.y       = 0;
        ev.buttons = 0;
        gui_window_push_event(win_list, &ev);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Compositor task entry point
 * ═══════════════════════════════════════════════════════════════════════════ */

void compositor_task(void)
{
    for (;;) {
        process_mouse();
        process_keyboard();

        if (compositor_quit) {
            /* Clear screen and tear down */
            if (fb_hw)
                memset(fb_hw, 0, (uint32_t)fb_h * fb_pitch);
            compositor_quit  = false;
            compositor_dirty = false;
            scene_dirty      = true;
            prev_cursor_x    = 0xFFFFFFFF;
            prev_cursor_y    = 0xFFFFFFFF;
            back             = NULL;
            fb               = fb_hw;
            struct task *self = compositor_task_ptr;
            compositor_task_ptr = NULL;
            disable_interrupts();
            task_kill(self); /* kill self — does not return */
        }

        /* Detect cursor movement */
        if (mouse_x != prev_cursor_x || mouse_y != prev_cursor_y)
            compositor_dirty = true;

        if (compositor_dirty) {
            compositor_dirty = false;
            composite();
            prev_cursor_x = mouse_x;
            prev_cursor_y = mouse_y;
        }

        /* Yield to other tasks — hlt until next IRQ fires */
        enable_interrupts();
        asm volatile("hlt");
        disable_interrupts();
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * gui_wake: called from IRQ handlers and syscalls to re-activate compositor
 * ═══════════════════════════════════════════════════════════════════════════ */

void gui_wake(void)
{
    if (!compositor_task_ptr) return;
    compositor_dirty = true;
}

void gui_wake_scene(void)
{
    scene_dirty = true;
    gui_wake();
}

bool gui_has_windows(void)
{
    return win_list != NULL;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Public window API
 * ═══════════════════════════════════════════════════════════════════════════ */

int gui_window_create(int32_t x, int32_t y, uint32_t w, uint32_t h,
                      const char *title)
{
    /* Start compositor on first window creation */
    gui_start_compositor();

    /* Find a free slot */
    struct window *slot = NULL;
    for (int i = 0; i < GUI_MAX_WINDOWS; i++) {
        if (win_pool[i].id == 0) {
            slot = &win_pool[i];
            break;
        }
    }
    if (!slot) return -1;

    uint32_t cw = w > (uint32_t)(GUI_BORDER * 2) ? w - (uint32_t)(GUI_BORDER * 2) : 0;
    uint32_t ch = h > (uint32_t)(GUI_TITLE_HEIGHT + GUI_BORDER * 2)
                  ? h - (uint32_t)(GUI_TITLE_HEIGHT + GUI_BORDER * 2) : 0;

    uint32_t bb_bytes = cw * ch * sizeof(uint32_t);
    uint32_t *bb = NULL;
    if (bb_bytes > 0) {
        bb = (uint32_t *)kmalloc(bb_bytes);
        if (!bb) return -1;
        /* Default client background */
        for (uint32_t i = 0; i < cw * ch; i++)
            bb[i] = COL_CLIENT_BG;
    }

    slot->id        = next_id++;
    slot->x         = x;
    slot->y         = y;
    slot->w         = w;
    slot->h         = h;
    slot->backbuffer = bb;
    slot->visible   = true;
    slot->focused   = false;
    slot->dirty     = true;
    slot->ev_head   = 0;
    slot->ev_tail   = 0;

    /* Copy title (truncate to fit) */
    unsigned int tlen = 0;
    while (title[tlen] && tlen < sizeof(slot->title) - 1)
        tlen++;
    memcpy(slot->title, title, tlen);
    slot->title[tlen] = '\0';

    /* Focus the new window (push to front) */
    if (win_list) win_list->focused = false;
    win_list_push_front(slot);
    slot->focused = true;


    gui_wake_scene();
    return slot->id;
}

void gui_window_destroy(int id)
{
    struct window *w = find_window(id);
    if (!w) return;

    win_list_remove(w);

    if (w->backbuffer) {
        kfree(w->backbuffer);
        w->backbuffer = NULL;
    }
    w->id = 0;  /* mark slot as free */

    /* Focus the new topmost window, or quit compositor if none left */
    if (win_list) {
        win_list->focused = true;
        gui_wake_scene();
    } else {
        compositor_quit = true;
        compositor_dirty = true;
    }
}

void gui_window_fill_rect(int id, uint32_t x, uint32_t y,
                          uint32_t w, uint32_t h, uint32_t color)
{
    struct window *win = find_window(id);
    if (!win) return;
    bb_fill_rect(win, x, y, w, h, color);
}

void gui_window_draw_rect(int id, uint32_t x, uint32_t y,
                          uint32_t w, uint32_t h, uint32_t color)
{
    struct window *win = find_window(id);
    if (!win) return;
    bb_draw_rect_outline(win, x, y, w, h, color);
}

void gui_window_draw_text(int id, uint32_t x, uint32_t y,
                          const char *str, uint32_t fgcolor, uint32_t bgcolor)
{
    struct window *win = find_window(id);
    if (!win) return;
    bb_draw_text(win, x, y, str, fgcolor, bgcolor);
}

void gui_window_flip(int id)
{
    struct window *win = find_window(id);
    if (!win) return;
    win->dirty = true;
    gui_wake_scene();
}

int gui_window_poll_event(int id, struct gui_event *ev)
{
    struct window *win = find_window(id);
    if (!win) return 0;
    if (win->ev_head == win->ev_tail) return 0;
    *ev = win->events[win->ev_tail];
    win->ev_tail = (win->ev_tail + 1) % GUI_EVENT_QUEUE_SIZE;
    return 1;
}

void gui_window_push_event(struct window *w, struct gui_event *ev)
{
    unsigned int next = (w->ev_head + 1) % GUI_EVENT_QUEUE_SIZE;
    if (next == w->ev_tail) return; /* full, drop */
    w->events[w->ev_head] = *ev;
    w->ev_head = next;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Initialisation
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Lazily start the compositor task on the first window creation */
static void gui_start_compositor(void)
{
    if (compositor_task_ptr) return; /* already running */

    /* Allocate full-screen back buffer from PMM frames.
     * Frames may be above the 8MB identity-mapped region, so explicitly
     * map each one at its physical address (identity map) so the kernel
     * can write to them. */
    uint32_t back_size = (uint32_t)fb_h * fb_pitch;
    uint32_t nframes   = (back_size + PAGE_SIZE - 1) / PAGE_SIZE;
    uint32_t first     = pmm_alloc_frame();
    if (first) {
        vmm_map_page(first, first, PTE_PRESENT | PTE_WRITE);
        uint32_t ok = 1;
        for (uint32_t i = 1; i < nframes; i++) {
            uint32_t f = pmm_alloc_frame();
            if (f != first + i * PAGE_SIZE) { ok = 0; break; }
            vmm_map_page(f, f, PTE_PRESENT | PTE_WRITE);
        }
        if (ok) {
            back = (char *)first;
            memset(back, 0, back_size);
            fb = back;
            printk("gui: back buffer at 0x%lx (%ld KB)\n", first, back_size / 1024);
        } else {
            printk("gui: back buffer frames not contiguous, drawing direct\n");
            back = NULL;
        }
    } else {
        printk("gui: back buffer pmm alloc failed, drawing direct\n");
        back = NULL;
    }

    static struct task comp_task;
    create_task(&comp_task, "compositor", 9, compositor_task);
    compositor_task_ptr = &comp_task;

    printk("gui: compositor started (%ldx%ld %d bpp)\n",
           (uint32_t)fb_w, (uint32_t)fb_h, fb_depth);
}

void init_gui(void)
{
    if (!fbdev_is_ready()) {
        printk("gui: no framebuffer, GUI disabled\n");
        return;
    }

    struct vbe_mode_info_struct *mode = vbe_get_mode_info();
    if (!mode) {
        printk("gui: no VBE mode info, GUI disabled\n");
        return;
    }

    /* Grab framebuffer parameters from VBE */
    fb_hw    = (char *)mode->framebuffer;
    fb       = fb_hw;   /* draw direct until back buffer is allocated */
    fb_w     = mode->width;
    fb_h     = mode->height;
    fb_pitch = mode->pitch;
    fb_depth = mode->bpp;
    fb_bpp   = fb_depth / 8;

    memset(win_pool, 0, sizeof(win_pool));
    win_list = NULL;
    next_id  = 1;

    /* Compositor is NOT started here — it starts lazily on first win_create */
    printk("gui: subsystem ready (%ldx%ld %d bpp), compositor not yet running\n",
           (uint32_t)fb_w, (uint32_t)fb_h, fb_depth);
}
