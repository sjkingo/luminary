/* Compositor / window manager kernel task.
 *
 * Architecture:
 *   - Window list: singly-linked list of struct window, z-ordered (head = topmost)
 *   - Each window has a client-area backbuffer (w × client_h × 4 bytes) in PMM frames
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
static uint32_t back_nframes;  /* number of PMM frames backing 'back' (for free on exit) */
static uint32_t fb_w;
static uint32_t fb_h;
static uint16_t fb_pitch;
static uint8_t  fb_depth;      /* bits per pixel (always 32 here) */
static uint32_t fb_bpp;        /* bytes per pixel */

/* ── forward declarations ────────────────────────────────────────────────── */
static void gui_start_compositor(void);
static void draw_statusbar(void);
static void window_realloc_backbuffer(struct window *w);

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

/* ── statusbar ───────────────────────────────────────────────────────────── */
#define STATUSBAR_H     22
#define COL_STATUSBAR   rgb(30, 30, 30)
#define COL_SB_TEXT     rgb(255, 255, 255)
#define COL_SB_CLOSE    rgb(200, 60, 60)
#define SB_CLOSE_W      54   /* width of "Close" button on right */

/* ── drag state ──────────────────────────────────────────────────────────── */
static struct window *drag_win    = NULL;
static int32_t        drag_off_x  = 0;
static int32_t        drag_off_y  = 0;

/* ── resize state ────────────────────────────────────────────────────────── */
#define RESIZE_EDGE_NONE   0
#define RESIZE_EDGE_LEFT   (1 << 0)
#define RESIZE_EDGE_RIGHT  (1 << 1)
#define RESIZE_EDGE_TOP    (1 << 2)
#define RESIZE_EDGE_BOTTOM (1 << 3)
#define RESIZE_HIT_PX      6   /* pixels inside/outside border that count as edge */
#define WIN_MIN_W          120
#define WIN_MIN_H          (GUI_TITLE_HEIGHT + GUI_BORDER * 2 + 24)

static struct window *resize_win   = NULL;
static int            resize_edges = RESIZE_EDGE_NONE;
static int32_t        resize_orig_x, resize_orig_y;
static uint32_t       resize_orig_w, resize_orig_h;
static uint32_t       resize_mouse_x0, resize_mouse_y0;

/* ── previous mouse button state (for edge detection) ───────────────────── */
static uint8_t prev_buttons = 0;

/* ── cursor sprites ──────────────────────────────────────────────────────── */
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

/* Resize cursor: horizontal (↔), 11×7, hotspot centred */
#define CURSOR_RESIZE_H_W 11
#define CURSOR_RESIZE_H_H  7
#define CURSOR_RESIZE_H_OX 5   /* hotspot x offset */
#define CURSOR_RESIZE_H_OY 3   /* hotspot y offset */
static const uint8_t cursor_resize_h[CURSOR_RESIZE_H_H][CURSOR_RESIZE_H_W] = {
    {0,0,0,1,0,0,0,1,0,0,0},
    {0,0,1,1,0,0,0,1,1,0,0},
    {0,1,1,1,1,1,1,1,1,1,0},
    {1,1,1,1,1,1,1,1,1,1,1},
    {0,1,1,1,1,1,1,1,1,1,0},
    {0,0,1,1,0,0,0,1,1,0,0},
    {0,0,0,1,0,0,0,1,0,0,0},
};

/* Resize cursor: vertical (↕), 7×11, hotspot centred */
#define CURSOR_RESIZE_V_W  7
#define CURSOR_RESIZE_V_H 11
#define CURSOR_RESIZE_V_OX 3
#define CURSOR_RESIZE_V_OY 5
static const uint8_t cursor_resize_v[CURSOR_RESIZE_V_H][CURSOR_RESIZE_V_W] = {
    {0,0,0,1,0,0,0},
    {0,0,1,1,1,0,0},
    {0,1,1,1,1,1,0},
    {0,0,0,1,0,0,0},
    {0,0,0,1,0,0,0},
    {1,1,1,1,1,1,1},
    {0,0,0,1,0,0,0},
    {0,0,0,1,0,0,0},
    {0,1,1,1,1,1,0},
    {0,0,1,1,1,0,0},
    {0,0,0,1,0,0,0},
};

/* Resize cursor: diagonal (↘/↗), 11×11, hotspot centred */
#define CURSOR_RESIZE_D_W 11
#define CURSOR_RESIZE_D_H 11
#define CURSOR_RESIZE_D_OX 5
#define CURSOR_RESIZE_D_OY 5
static const uint8_t cursor_resize_d[CURSOR_RESIZE_D_H][CURSOR_RESIZE_D_W] = {
    {1,1,1,1,1,0,0,0,0,0,0},
    {1,1,1,1,0,0,0,0,0,0,0},
    {1,1,1,0,0,0,0,0,0,0,1},
    {1,1,0,0,0,0,0,0,0,1,1},
    {1,0,0,0,0,0,0,0,1,1,1},
    {0,0,0,0,0,0,0,0,0,0,0},
    {1,0,0,0,1,0,0,0,0,0,1},
    {1,1,0,1,0,0,0,0,0,1,1},
    {1,0,0,0,0,0,0,0,1,1,1},
    {0,0,0,0,0,0,0,1,1,1,1},
    {0,0,0,0,0,0,1,1,1,1,1},
};

/* Which resize cursor to show (set each frame based on hover) */
#define CURSOR_TYPE_NORMAL  0
#define CURSOR_TYPE_H       1
#define CURSOR_TYPE_V       2
#define CURSOR_TYPE_D       3
static int cursor_type = CURSOR_TYPE_NORMAL;

/* Hover state for focus-follows-mouse */
static struct window *hovered_win = NULL;

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
 * Window backbuffer allocation (PMM frames, identity-mapped)
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Allocate contiguous PMM frames for a backbuffer of cw×ch pixels.
 * Maps each frame into the kernel page directory.
 * Returns pointer to the buffer, or NULL on failure.
 * Sets *nframes_out to the number of frames allocated. */
static uint32_t *bb_alloc(uint32_t cw, uint32_t ch, uint32_t *nframes_out)
{
    uint32_t bytes   = cw * ch * sizeof(uint32_t);
    uint32_t nframes = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    *nframes_out = 0;
    void *p = vmm_alloc_pages(nframes);
    if (!p) return NULL;
    *nframes_out = nframes;
    return (uint32_t *)p;
}

static void bb_free(uint32_t *buf, uint32_t nframes)
{
    vmm_free_pages(buf, nframes);
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

    /* Only blit backbuffer when its allocated dimensions match the current
     * client size. During a live resize they'll differ — just fill background. */
    if (w->backbuffer && cw > 0 && ch > 0 && w->bb_w == cw && w->bb_h == ch) {
        /* Clip client area against screen bounds */
        int32_t src_x0 = 0;
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

/* Draw cursor directly into fb_hw (never into back, so back stays clean) */
static inline void hw_putpixel(int32_t x, int32_t y, uint32_t color)
{
    if (x < 0 || y < 0 || (uint32_t)x >= fb_w || (uint32_t)y >= fb_h) return;
    *(uint32_t *)(fb_hw + y * fb_pitch + x * fb_bpp) = color;
}

#define DRAW_CURSOR_SPRITE_HW(mask, rows, cols, ox, oy) do { \
    int32_t cx = (int32_t)mouse_x - (ox); \
    int32_t cy = (int32_t)mouse_y - (oy); \
    for (int ddy = -1; ddy <= 1; ddy++) { \
        for (int ddx = -1; ddx <= 1; ddx++) { \
            if (ddx == 0 && ddy == 0) continue; \
            for (uint32_t r = 0; r < (rows); r++) \
                for (uint32_t c = 0; c < (cols); c++) \
                    if ((mask)[r][c]) \
                        hw_putpixel(cx+(int32_t)c+ddx, cy+(int32_t)r+ddy, COL_CURSOR_OUT); \
        } \
    } \
    for (uint32_t r = 0; r < (rows); r++) \
        for (uint32_t c = 0; c < (cols); c++) \
            if ((mask)[r][c]) \
                hw_putpixel(cx+(int32_t)c, cy+(int32_t)r, COL_CURSOR); \
} while(0)

/* Draw cursor directly into fb_hw */
static void draw_cursor(void)
{
    if (!fb_hw) return;
    switch (cursor_type) {
        case CURSOR_TYPE_H:
            DRAW_CURSOR_SPRITE_HW(cursor_resize_h, CURSOR_RESIZE_H_H, CURSOR_RESIZE_H_W,
                                  CURSOR_RESIZE_H_OX, CURSOR_RESIZE_H_OY);
            break;
        case CURSOR_TYPE_V:
            DRAW_CURSOR_SPRITE_HW(cursor_resize_v, CURSOR_RESIZE_V_H, CURSOR_RESIZE_V_W,
                                  CURSOR_RESIZE_V_OX, CURSOR_RESIZE_V_OY);
            break;
        case CURSOR_TYPE_D:
            DRAW_CURSOR_SPRITE_HW(cursor_resize_d, CURSOR_RESIZE_D_H, CURSOR_RESIZE_D_W,
                                  CURSOR_RESIZE_D_OX, CURSOR_RESIZE_D_OY);
            break;
        default:
            DRAW_CURSOR_SPRITE_HW(cursor_mask, CURSOR_H, CURSOR_W, 0, 0);
            break;
    }
}

/* Blit rows from back buffer to hw, then redraw cursor if it overlaps.
 * This naturally erases the old cursor (back has no cursor in it). */
static inline void blit_rows(uint32_t y_start, uint32_t y_end)
{
    if (!back || !fb_hw || back == fb_hw) return;
    if (y_end > fb_h) y_end = fb_h;
    if (y_start >= y_end) return;
    memcpy(fb_hw + y_start * fb_pitch,
           back   + y_start * fb_pitch,
           (y_end - y_start) * fb_pitch);
    /* Redraw cursor into hw if it overlaps the blitted rows */
    uint32_t cy0 = (mouse_y >= 1 ? mouse_y - 1 : 0);
    uint32_t cy1 = mouse_y + CURSOR_H + 1;
    if (cy1 > fb_h) cy1 = fb_h;
    if (cy0 < y_end && cy1 > y_start)
        draw_cursor();
}

/* scene_dirty: set when windows/desktop need full redraw (not just cursor) */
static bool scene_dirty = true;

/* Full composite: draw clean scene into back, blit to hw, draw cursor on hw */
static void composite(void)
{
    if (scene_dirty) {
        /* Full scene redraw into back (no cursor — back stays clean) */
        draw_desktop();

        struct window *stack[GUI_MAX_WINDOWS];
        int depth = 0;
        struct window *w = win_list;
        while (w && depth < GUI_MAX_WINDOWS) {
            if (w->visible) stack[depth++] = w;
            w = w->next;
        }
        for (int i = depth - 1; i >= 0; i--) {
            stack[i]->dirty = false;
            draw_window(stack[i]);
        }

        draw_statusbar();
        scene_dirty = false;

        /* Blit clean back → hw, then draw cursor directly on hw */
        if (back && fb_hw && back != fb_hw)
            memcpy(fb_hw, back, (uint32_t)fb_h * fb_pitch);
        draw_cursor();
    } else {
        /* Partial update: dirty windows and/or cursor movement.
         * back stays clean (no cursor). blit_rows copies back→hw and
         * redraws cursor on hw wherever it overlaps the blitted region. */

        struct window *stack[GUI_MAX_WINDOWS];
        int depth = 0;
        struct window *w = win_list;
        while (w && depth < GUI_MAX_WINDOWS) {
            if (w->visible) stack[depth++] = w;
            w = w->next;
        }

        /* Repaint dirty windows into back, track affected rows */
        uint32_t dirty_top    = fb_h;
        uint32_t dirty_bottom = 0;
        for (int i = depth - 1; i >= 0; i--) {
            struct window *ww = stack[i];
            if (!ww->dirty) continue;
            ww->dirty = false;
            draw_window(ww);
            uint32_t wy0 = ww->y < 0 ? 0 : (uint32_t)ww->y;
            uint32_t wy1 = (uint32_t)(ww->y + (int32_t)ww->h);
            if (wy1 > fb_h) wy1 = fb_h;
            if (wy0 < dirty_top)    dirty_top    = wy0;
            if (wy1 > dirty_bottom) dirty_bottom = wy1;
        }

        /* Cursor erase: blit old cursor rows from back (which has no cursor)
         * to hw, covering the old cursor pixels with clean scene content.
         * Then add new cursor position rows to the blit range. */
        uint32_t old_y = prev_cursor_y;
        uint32_t new_y = mouse_y;
        uint32_t cur_top    = (old_y >= 1 ? old_y - 1 : 0);
        uint32_t cur_bottom = old_y + CURSOR_H + 1;
        /* Expand to cover new cursor position too */
        uint32_t new_top    = (new_y >= 1 ? new_y - 1 : 0);
        uint32_t new_bottom = new_y + CURSOR_H + 1;
        if (new_top    < cur_top)    cur_top    = new_top;
        if (new_bottom > cur_bottom) cur_bottom = new_bottom;
        if (cur_bottom > fb_h) cur_bottom = fb_h;

        /* Merge cursor rows into dirty range */
        if (cur_top    < dirty_top)    dirty_top    = cur_top;
        if (cur_bottom > dirty_bottom) dirty_bottom = cur_bottom;

        /* Single blit covers both dirty windows and cursor erase/redraw.
         * blit_rows copies back→hw (clean, no cursor) then redraws cursor
         * on hw if it overlaps. */
        if (dirty_top < dirty_bottom)
            blit_rows(dirty_top, dirty_bottom);
    }
}

/* ── statusbar / taskbar ─────────────────────────────────────────────────── */
#define TASKBAR_BTN_PAD   4    /* px padding inside each taskbar button */
#define TASKBAR_BTN_GAP   2    /* px gap between buttons */
#define TASKBAR_BTN_MAX_W 140  /* max width of a single taskbar button */
#define COL_SB_BTN_FOCUS  rgb(70, 110, 190)
#define COL_SB_BTN_NORM   rgb(55,  55,  55)
#define COL_SB_BTN_BORDER rgb(90,  90,  90)

/* Compute taskbar button x position and width for window index i out of n.
 * Buttons are left-aligned, capped at TASKBAR_BTN_MAX_W, leaving room for
 * the close button on the right. */
static void taskbar_btn_rect(int i, int n, int32_t *bx, uint32_t *bw)
{
    (void)n;
    uint32_t w = TASKBAR_BTN_MAX_W;
    *bx = TASKBAR_BTN_GAP + i * (int32_t)(w + TASKBAR_BTN_GAP);
    *bw = w;
}

static void draw_statusbar(void)
{
    /* Background strip */
    fb_fill_rect(0, 0, fb_w, STATUSBAR_H, COL_STATUSBAR);

    /* Close button on the right — always visible */
    int32_t close_x = (int32_t)fb_w - SB_CLOSE_W;
    fb_fill_rect(close_x, 0, SB_CLOSE_W, STATUSBAR_H, COL_SB_CLOSE);
    int32_t lx = close_x + (SB_CLOSE_W - (int32_t)(5 * CONSOLE_FONT_WIDTH)) / 2;
    int32_t ly = (STATUSBAR_H - CONSOLE_FONT_HEIGHT) / 2;
    fb_draw_text("Close", lx, ly, COL_SB_TEXT, COL_SB_CLOSE, false);

    /* Taskbar: one button per window, drawn back-to-front (so list order =
     * z-order), but we want left-to-right in creation order. Collect first. */
    struct window *stack[GUI_MAX_WINDOWS];
    int n = 0;
    struct window *w = win_list;
    while (w && n < GUI_MAX_WINDOWS) {
        if (w->visible) stack[n++] = w;
        w = w->next;
    }
    /* Reverse so oldest (bottom of z-stack) is leftmost */
    for (int i = 0, j = n - 1; i < j; i++, j--) {
        struct window *tmp = stack[i]; stack[i] = stack[j]; stack[j] = tmp;
    }

    int32_t ty = (STATUSBAR_H - CONSOLE_FONT_HEIGHT) / 2;
    for (int i = 0; i < n; i++) {
        int32_t bx; uint32_t bw;
        taskbar_btn_rect(i, n, &bx, &bw);
        if (bx + (int32_t)bw >= close_x) break; /* no room */

        uint32_t bg = stack[i]->focused ? COL_SB_BTN_FOCUS : COL_SB_BTN_NORM;
        fb_fill_rect(bx, 1, bw, STATUSBAR_H - 2, bg);
        fb_draw_rect_outline(bx, 1, bw, STATUSBAR_H - 2, COL_SB_BTN_BORDER);

        /* Truncate title to fit button width */
        const char *title = stack[i]->title;
        uint32_t max_chars = (bw - 2 * TASKBAR_BTN_PAD) / CONSOLE_FONT_WIDTH;
        uint32_t tlen = 0;
        while (title[tlen]) tlen++;
        if (tlen > max_chars) tlen = max_chars;

        /* Draw char by char up to tlen */
        int32_t tx = bx + TASKBAR_BTN_PAD;
        for (uint32_t c = 0; c < tlen; c++) {
            fb_draw_char((uint8_t)title[c], tx, ty, COL_SB_TEXT, bg, false);
            tx += CONSOLE_FONT_WIDTH;
        }
    }
}

/* Returns which resize edges (bitmask) the point (px,py) hits on window w,
 * or RESIZE_EDGE_NONE if not on a border. */
static int resize_hit_test(struct window *w, uint32_t px, uint32_t py)
{
    int32_t x0 = w->x;
    int32_t y0 = w->y;
    int32_t x1 = w->x + (int32_t)w->w;
    int32_t y1 = w->y + (int32_t)w->h;
    int32_t p  = RESIZE_HIT_PX;
    int32_t ix = (int32_t)px;
    int32_t iy = (int32_t)py;

    /* Must be within the outer hit zone */
    if (ix < x0 - p || ix >= x1 + p || iy < y0 - p || iy >= y1 + p)
        return RESIZE_EDGE_NONE;
    /* Must not be deep inside (past the border + hit zone on all sides) */
    if (ix >= x0 + p && ix < x1 - p && iy >= y0 + p && iy < y1 - p)
        return RESIZE_EDGE_NONE;

    int edges = RESIZE_EDGE_NONE;
    if (ix < x0 + p) edges |= RESIZE_EDGE_LEFT;
    if (ix >= x1 - p) edges |= RESIZE_EDGE_RIGHT;
    if (iy < y0 + p) edges |= RESIZE_EDGE_TOP;
    if (iy >= y1 - p) edges |= RESIZE_EDGE_BOTTOM;
    return edges;
}

/* Reallocate a window's backbuffer to match its current w/h.
 * Called after a resize completes. */
static void window_realloc_backbuffer(struct window *w)
{
    uint32_t cw = client_w(w);
    uint32_t ch = client_h(w);
    bb_free(w->backbuffer, w->bb_nframes);
    w->backbuffer = NULL;
    w->bb_nframes = 0;
    if (cw > 0 && ch > 0) {
        uint32_t nframes;
        uint32_t *bb = bb_alloc(cw, ch, &nframes);
        if (bb) {
            for (uint32_t i = 0; i < cw * ch; i++)
                bb[i] = COL_CLIENT_BG;
            w->backbuffer = bb;
            w->bb_w       = cw;
            w->bb_h       = ch;
            w->bb_nframes = nframes;
        }
    }
    w->dirty = true;
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

        uint8_t pressed  = ev.buttons & ~prev_buttons;
        uint8_t released = ~ev.buttons & prev_buttons;
        prev_buttons = ev.buttons;

        /* ── active drag ── */
        if (drag_win && (ev.buttons & MOUSE_BTN_LEFT)) {
            drag_win->x = (int32_t)px - drag_off_x;
            drag_win->y = (int32_t)py - drag_off_y;
            scene_dirty = true;
        }

        /* ── active resize ── */
        if (resize_win && (ev.buttons & MOUSE_BTN_LEFT)) {
            int32_t ddx = (int32_t)px - (int32_t)resize_mouse_x0;
            int32_t ddy = (int32_t)py - (int32_t)resize_mouse_y0;
            int32_t nx = resize_orig_x;
            int32_t ny = resize_orig_y;
            int32_t nw = (int32_t)resize_orig_w;
            int32_t nh = (int32_t)resize_orig_h;

            if (resize_edges & RESIZE_EDGE_RIGHT)  nw += ddx;
            if (resize_edges & RESIZE_EDGE_BOTTOM) nh += ddy;
            if (resize_edges & RESIZE_EDGE_LEFT)  { nx += ddx; nw -= ddx; }
            if (resize_edges & RESIZE_EDGE_TOP)   { ny += ddy; nh -= ddy; }

            if (nw < WIN_MIN_W) { if (resize_edges & RESIZE_EDGE_LEFT) nx -= WIN_MIN_W - nw; nw = WIN_MIN_W; }
            if (nh < WIN_MIN_H) { if (resize_edges & RESIZE_EDGE_TOP)  ny -= WIN_MIN_H - nh; nh = WIN_MIN_H; }

            if (nx != resize_win->x || ny != resize_win->y ||
                (uint32_t)nw != resize_win->w || (uint32_t)nh != resize_win->h) {
                resize_win->x = nx;
                resize_win->y = ny;
                resize_win->w = (uint32_t)nw;
                resize_win->h = (uint32_t)nh;
                /* Don't realloc backbuffer mid-drag — contents stay visible,
                 * clipped by draw_window. Realloc happens on release. */
                scene_dirty = true;
            }
        }

        /* ── left button pressed ── */
        if (pressed & MOUSE_BTN_LEFT) {
            /* Statusbar / taskbar clicks */
            if (py < STATUSBAR_H) {
                if ((int32_t)px >= (int32_t)fb_w - SB_CLOSE_W) {
                    /* Close button: destroy focused window */
                    if (win_list)
                        gui_window_destroy(win_list->id);
                    drag_win = NULL; resize_win = NULL;
                } else {
                    /* Taskbar buttons: find which window was clicked */
                    struct window *stack[GUI_MAX_WINDOWS];
                    int n = 0;
                    struct window *tw = win_list;
                    while (tw && n < GUI_MAX_WINDOWS) {
                        if (tw->visible) stack[n++] = tw;
                        tw = tw->next;
                    }
                    /* Reverse to match draw order */
                    for (int i = 0, j = n-1; i < j; i++, j--) {
                        struct window *tmp = stack[i]; stack[i] = stack[j]; stack[j] = tmp;
                    }
                    int32_t close_x = (int32_t)fb_w - SB_CLOSE_W;
                    for (int i = 0; i < n; i++) {
                        int32_t bx; uint32_t bw;
                        taskbar_btn_rect(i, n, &bx, &bw);
                        if (bx + (int32_t)bw >= close_x) break;
                        if ((int32_t)px >= bx && (int32_t)px < bx + (int32_t)bw) {
                            focus_window(stack[i]);
                            scene_dirty = true;
                            break;
                        }
                    }
                }
                drag_win = NULL; resize_win = NULL;
                goto next_event;
            }

            struct window *hit = hit_test(px, py);
            if (hit) {
                focus_window(hit);

                /* Check resize edges first (before title bar / client) */
                int edges = resize_hit_test(hit, px, py);
                if (edges != RESIZE_EDGE_NONE && !in_title_bar(hit, px, py)) {
                    resize_win     = hit;
                    resize_edges   = edges;
                    resize_orig_x  = hit->x;
                    resize_orig_y  = hit->y;
                    resize_orig_w  = hit->w;
                    resize_orig_h  = hit->h;
                    resize_mouse_x0 = px;
                    resize_mouse_y0 = py;
                } else if (in_close_btn(hit, px, py)) {
                    gui_window_destroy(hit->id);
                    drag_win   = NULL;
                    resize_win = NULL;
                } else if (in_title_bar(hit, px, py)) {
                    drag_win   = hit;
                    drag_off_x = (int32_t)px - hit->x;
                    drag_off_y = (int32_t)py - hit->y;
                } else {
                    /* Click in client area */
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

        /* ── left button released ── */
        if (released & MOUSE_BTN_LEFT) {
            drag_win = NULL;
            if (resize_win) {
                window_realloc_backbuffer(resize_win);
                struct gui_event rev;
                rev.type    = GUI_EVENT_RESIZE;
                rev.x       = (uint16_t)client_w(resize_win);
                rev.y       = (uint16_t)client_h(resize_win);
                rev.key     = 0;
                rev.buttons = 0;
                gui_window_push_event(resize_win, &rev);
                resize_win = NULL;
            }
        }

        next_event:;
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
            cursor_type      = CURSOR_TYPE_NORMAL;
            hovered_win      = NULL;
            /* back buffer is owned by init_gui(), not freed here */
            fb               = fb_hw;
            struct task *self = compositor_task_ptr;
            compositor_task_ptr = NULL;
            disable_interrupts();
            task_kill(self); /* kill self — does not return */
        }

        /* Detect cursor movement */
        if (mouse_x != prev_cursor_x || mouse_y != prev_cursor_y) {
            compositor_dirty = true;

            /* Update cursor shape and focus-follows-mouse */
            uint32_t px = mouse_x, py = mouse_y;
            int new_cursor = CURSOR_TYPE_NORMAL;

            if (py >= STATUSBAR_H) {
                struct window *w = hit_test(px, py);

                /* Focus on hover (focus-follows-mouse) */
                if (w != hovered_win) {
                    hovered_win = w;
                    if (w && w != win_list) {
                        focus_window(w);
                        scene_dirty = true;
                    }
                }

                /* Cursor shape: check resize edges of topmost window */
                if (w && !resize_win && !drag_win) {
                    int edges = resize_hit_test(w, px, py);
                    bool is_h  = (edges & (RESIZE_EDGE_LEFT | RESIZE_EDGE_RIGHT)) &&
                                 !(edges & (RESIZE_EDGE_TOP | RESIZE_EDGE_BOTTOM));
                    bool is_v  = (edges & (RESIZE_EDGE_TOP | RESIZE_EDGE_BOTTOM)) &&
                                 !(edges & (RESIZE_EDGE_LEFT | RESIZE_EDGE_RIGHT));
                    bool is_d  = (edges & (RESIZE_EDGE_LEFT | RESIZE_EDGE_RIGHT)) &&
                                 (edges & (RESIZE_EDGE_TOP | RESIZE_EDGE_BOTTOM));
                    if (is_h) new_cursor = CURSOR_TYPE_H;
                    else if (is_v) new_cursor = CURSOR_TYPE_V;
                    else if (is_d) new_cursor = CURSOR_TYPE_D;
                }
            }

            if (new_cursor != cursor_type) {
                cursor_type = new_cursor;
                /* Old cursor erased by blit_rows copying clean back over hw */
            }
        }

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

    uint32_t nframes = 0;
    uint32_t *bb = NULL;
    if (cw > 0 && ch > 0) {
        bb = bb_alloc(cw, ch, &nframes);
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
    slot->bb_w       = cw;
    slot->bb_h       = ch;
    slot->bb_nframes = nframes;
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

    bb_free(w->backbuffer, w->bb_nframes);
    w->backbuffer = NULL;
    w->bb_nframes = 0;
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
    gui_wake();
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

int gui_window_get_size(int id, uint32_t *cw, uint32_t *ch)
{
    struct window *w = find_window(id);
    if (!w) return -1;
    *cw = client_w(w);
    *ch = client_h(w);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Initialisation
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Lazily start the compositor task on the first window creation */
static void gui_start_compositor(void)
{
    if (compositor_task_ptr) return; /* already running */

    /* back buffer was pre-allocated in init_gui(); just activate it */
    if (back) {
        uint32_t back_size = (uint32_t)fb_h * fb_pitch;
        memset(back, 0, back_size);
        fb = back;
        DBGK("gui", "back buffer at 0x%lx (%ld KB)\n",
             (uint32_t)back, back_size / 1024);
    } else {
        DBGK("gui", "no back buffer, drawing direct\n");
    }

    static struct task comp_task;
    create_task(&comp_task, "compositor", 9, compositor_task);
    compositor_task_ptr = &comp_task;

    DBGK("gui", "compositor started (%ldx%ld %d bpp)\n",
         (uint32_t)fb_w, (uint32_t)fb_h, fb_depth);
}

void init_gui(void)
{
    if (!fbdev_is_ready()) {
        DBGK("gui", "no framebuffer, GUI disabled\n");
        return;
    }

    struct vbe_mode_info_struct *mode = vbe_get_mode_info();
    if (!mode) {
        DBGK("gui", "no VBE mode info, GUI disabled\n");
        return;
    }

    /* Grab framebuffer parameters from VBE */
    fb_hw    = (char *)mode->framebuffer;
    fb       = fb_hw;   /* draw direct until compositor starts */
    fb_w     = mode->width;
    fb_h     = mode->height;
    fb_pitch = mode->pitch;
    fb_depth = mode->bpp;
    fb_bpp   = fb_depth / 8;

    /* Allocate the back buffer now, while PMM is unfragmented, so frames
     * are physically contiguous and identity-mapped (fast memcpy). */
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
            back_nframes = nframes;
            DBGK("gui", "back buffer reserved at 0x%lx (%ld KB)\n",
                 first, back_size / 1024);
        } else {
            /* Shouldn't happen this early, but fall back gracefully */
            back = NULL;
            back_nframes = 0;
            DBGK("gui", "back buffer frames not contiguous\n");
        }
    }

    memset(win_pool, 0, sizeof(win_pool));
    win_list = NULL;
    next_id  = 1;

    DBGK("gui", "subsystem ready (%ldx%ld %d bpp)\n",
         (uint32_t)fb_w, (uint32_t)fb_h, fb_depth);
}
