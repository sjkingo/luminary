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
#include "kernel/vfs.h"
#include "kernel/x.h"
#include "drivers/fbdev.h"
#include "drivers/vbe.h"
#include "drivers/mouse.h"
#include "drivers/keyboard.h"
#include "cpu/x86.h"
#include "fonts/ctrld16r.h"

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
static int32_t        drag_prev_x = 0;   /* window position at last drag frame */
static int32_t        drag_prev_y = 0;

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
#define COL_DESKTOP     rgb(0,   0,   0)

/* Runtime desktop fill colour — 0 means use COL_DESKTOP */
static uint32_t desktop_color = 0;
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

/* Allocate a backbuffer of cw×ch pixels via vmm_alloc_pages.
 * Physical frames need not be contiguous; they are mapped to a
 * contiguous virtual range in the kernel virtual allocator space.
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

/* Desktop background image (optional). Set via gui_set_bg().
 *
 * Both bg_raw and bg_scaled are pre-allocated at init_gui() time so that
 * gui_set_bg() (called from the syscall handler) never needs to do a large
 * kernel heap allocation at runtime.  Runtime large kmalloc/vmm_alloc_pages
 * calls from syscall context displace the VMM bump pointer into ranges
 * already occupied by kernel stacks, corrupting them.
 *
 * bg_raw_valid / bg_scaled_valid replace NULL-pointer checks.
 *
 * bg_raw must hold the largest image the wallpaper tool can send.
 * wallpaper.c caps at MAX_W=1280, MAX_H=1092 — match those here. */
#define BG_RAW_MAX_W   1280u
#define BG_RAW_MAX_H   1092u
static uint32_t *bg_raw       = NULL;   /* pre-allocated, BG_RAW_MAX_W×BG_RAW_MAX_H */
static bool      bg_raw_valid = false;  /* true when bg_raw holds new pixels */
static uint32_t  bg_raw_w     = 0;
static uint32_t  bg_raw_h     = 0;
static uint32_t  bg_raw_nframes = 0;

static uint32_t *bg_scaled       = NULL;  /* pre-allocated, screen-sized */
static bool      bg_scaled_valid = false;
static uint32_t  bg_dst_x  = 0;
static uint32_t  bg_dst_y  = 0;
static uint32_t  bg_dst_w  = 0;
static uint32_t  bg_dst_h  = 0;
static uint32_t  bg_scaled_nframes = 0;

/* Called from syscall handler — must not do heavy work. */
void gui_set_bg(const uint32_t *pixels, uint32_t w, uint32_t h)
{
    bg_raw_valid    = false;
    bg_scaled_valid = false;
    bg_dst_w = bg_dst_h = 0;

    if (!pixels || w == 0 || h == 0 || !bg_raw) { gui_wake_scene(); return; }

    uint32_t nbytes = w * h * sizeof(uint32_t);
    uint32_t capacity = bg_raw_nframes * PAGE_SIZE;
    if (nbytes > capacity) { gui_wake_scene(); return; }

    memcpy(bg_raw, pixels, nbytes);
    bg_raw_w     = w;
    bg_raw_h     = h;
    bg_raw_valid = true;
    gui_wake_scene();
}

/* Called from compositor task — interrupts enabled, safe for heavy work. */
static void bg_scale_pending(void)
{
    if (!bg_raw_valid) return;

    uint32_t w = bg_raw_w, h = bg_raw_h;

    /* Compute letterbox destination rectangle.
     * Fill the screen on one axis, preserve aspect ratio on the other.
     * If the scaled dimension exceeds the screen (e.g. near-square image on
     * a landscape screen), clamp to screen size and centre-crop. */
    uint32_t dst_x, dst_y, dst_w, dst_h;
    uint32_t full_w, full_h;   /* unclipped scaled size for source sampling */
    if (w * fb_h > h * fb_w) {
        full_h = fb_h; full_w = w * fb_h / h;
    } else {
        full_w = fb_w; full_h = h * fb_w / w;
    }

    dst_w = full_w < fb_w ? full_w : fb_w;
    dst_h = full_h < fb_h ? full_h : fb_h;
    dst_x = dst_w < fb_w ? (fb_w - dst_w) / 2 : 0;
    dst_y = dst_h < fb_h ? (fb_h - dst_h) / 2 : 0;

    /* x_off/y_off: where in the full scaled image our cropped window starts */
    uint32_t x_off = (full_w - dst_w) / 2;
    uint32_t y_off = (full_h - dst_h) / 2;

    if (!bg_scaled || dst_w * dst_h * sizeof(uint32_t) > bg_scaled_nframes * PAGE_SIZE)
        return;

    /* Enable interrupts during the scale loop so the timer IRQ fires
     * normally — this loop takes ~10ms and would otherwise stall everything. */
    enable_interrupts();
    for (uint32_t y = 0; y < dst_h; y++) {
        uint32_t src_y = (y + y_off) * h / full_h;
        uint32_t *dst_row = bg_scaled + y * dst_w;
        for (uint32_t x = 0; x < dst_w; x++)
            dst_row[x] = bg_raw[src_y * w + (x + x_off) * w / full_w];
    }
    disable_interrupts();

    bg_raw_valid    = false;
    bg_dst_x = dst_x; bg_dst_y = dst_y;
    bg_dst_w = dst_w; bg_dst_h = dst_h;
    bg_scaled_valid = true;
}

void gui_set_desktop_color(uint32_t r, uint32_t g, uint32_t b)
{
    desktop_color = rgb(r & 0xFF, g & 0xFF, b & 0xFF);
    gui_wake_scene();
}

/* Draw the desktop background — straight blit, no division */
static void draw_desktop(void)
{
    uint32_t fill = desktop_color ? desktop_color : COL_DESKTOP;

    if (!bg_scaled_valid) {
        fb_fill_rect(0, 0, fb_w, fb_h, fill);
        return;
    }

    /* Fill letterbox bars */
    if (bg_dst_y > 0)
        fb_fill_rect(0, 0, fb_w, bg_dst_y, fill);
    if (bg_dst_y + bg_dst_h < fb_h)
        fb_fill_rect(0, (int32_t)(bg_dst_y + bg_dst_h), fb_w, fb_h - bg_dst_y - bg_dst_h, fill);
    if (bg_dst_x > 0)
        fb_fill_rect(0, (int32_t)bg_dst_y, bg_dst_x, bg_dst_h, fill);
    if (bg_dst_x + bg_dst_w < fb_w)
        fb_fill_rect((int32_t)(bg_dst_x + bg_dst_w), (int32_t)bg_dst_y, fb_w - bg_dst_x - bg_dst_w, bg_dst_h, fill);

    /* Blit pre-scaled rows directly to framebuffer */
    for (uint32_t y = 0; y < bg_dst_h; y++) {
        uint32_t *src_row = bg_scaled + y * bg_dst_w;
        uint32_t *dst_row = (uint32_t *)(fb + (bg_dst_y + y) * fb_pitch + bg_dst_x * fb_bpp);
        memcpy(dst_row, src_row, bg_dst_w * sizeof(uint32_t));
    }
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

/* During drag/resize, track the row band that needs blitting to hw so we can
 * skip the full-screen back→hw memcpy. Set whenever scene_dirty is set from
 * the drag/resize handlers. Reset to invalid (top > bottom) otherwise. */
static uint32_t drag_blit_top    = 0;
static uint32_t drag_blit_bottom = 0;  /* 0 means: do full blit */

/* Full composite: draw clean scene into back, blit to hw, draw cursor on hw */
static void composite(void)
{
    /* Scale any pending raw bg image — deferred from syscall handler to here
     * so the ~1M-iteration loop runs in the compositor task (interrupts on). */
    if (bg_raw_valid) {
        bg_scale_pending();
        scene_dirty = true;
    }

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

        if (back && fb_hw && back != fb_hw) {
            if (drag_blit_bottom > drag_blit_top) {
                /* Drag/resize: only blit the rows that moved */
                blit_rows(drag_blit_top, drag_blit_bottom);
            } else {
                memcpy(fb_hw, back, (uint32_t)fb_h * fb_pitch);
                draw_cursor();
            }
        }
        drag_blit_top = drag_blit_bottom = 0;
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
            int32_t new_x = (int32_t)px - drag_off_x;
            int32_t new_y = (int32_t)py - drag_off_y;
            if (new_x != drag_win->x || new_y != drag_win->y) {
                /* Row band = union of old and new window rects */
                int32_t top = drag_prev_y < new_y ? drag_prev_y : new_y;
                int32_t bot_old = drag_prev_y + (int32_t)drag_win->h;
                int32_t bot_new = new_y       + (int32_t)drag_win->h;
                int32_t bot = bot_old > bot_new ? bot_old : bot_new;
                uint32_t utop = (top < 0) ? 0 : (uint32_t)top;
                uint32_t ubot = (bot < 0) ? 0 : (uint32_t)bot;
                if (ubot > fb_h) ubot = fb_h;
                /* Accumulate across multiple drag frames between composites */
                if (drag_blit_bottom == 0 && drag_blit_top == 0) {
                    drag_blit_top    = utop;
                    drag_blit_bottom = ubot;
                } else {
                    if (utop < drag_blit_top)    drag_blit_top    = utop;
                    if (ubot > drag_blit_bottom) drag_blit_bottom = ubot;
                }
                drag_win->x = new_x;
                drag_win->y = new_y;
                drag_prev_x = new_x;
                drag_prev_y = new_y;
                scene_dirty = true;
            }
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
                /* Row band: union of old and new rects */
                int32_t rtop = resize_win->y < ny ? resize_win->y : ny;
                int32_t rbot_old = resize_win->y + (int32_t)resize_win->h;
                int32_t rbot_new = ny + nh;
                int32_t rbot = rbot_old > rbot_new ? rbot_old : rbot_new;
                uint32_t utop = rtop < 0 ? 0 : (uint32_t)rtop;
                uint32_t ubot = rbot < 0 ? 0 : (uint32_t)rbot;
                if (ubot > fb_h) ubot = fb_h;
                if (drag_blit_bottom == 0 && drag_blit_top == 0) {
                    drag_blit_top = utop; drag_blit_bottom = ubot;
                } else {
                    if (utop < drag_blit_top)    drag_blit_top    = utop;
                    if (ubot > drag_blit_bottom) drag_blit_bottom = ubot;
                }
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
                    drag_prev_x = hit->x;
                    drag_prev_y = hit->y;
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
            /* Restore the framebuffer console: blank the screen first so the
             * GUI is gone, then repaint the text console from its ring buffer. */
            if (fb_hw)
                memset(fb_hw, 0, (uint32_t)fb_h * fb_pitch);
            fbdev_redraw();
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
    slot->owner_pid = running_task ? running_task->pid : 0;
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


    /* Take keyboard ownership on first window */
    if (!win_list->next)
        kbd_set_owner(1);

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
        /* Release keyboard ownership when last window closes */
        kbd_set_owner(0);
        compositor_quit = true;
        compositor_dirty = true;
    }
}

void gui_destroy_windows_for_pid(uint32_t pid)
{
    for (int i = 0; i < GUI_MAX_WINDOWS; i++) {
        if (win_pool[i].id != 0 && win_pool[i].owner_pid == pid)
            gui_window_destroy(win_pool[i].id);
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
    if (!win) {
        /* Window was destroyed — deliver a CLOSE event once */
        ev->type    = GUI_EVENT_CLOSE;
        ev->key     = 0;
        ev->x       = 0;
        ev->y       = 0;
        ev->buttons = 0;
        return 1;
    }
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
        DBGK("back buffer at 0x%lx (%ld KB)\n",
             (uint32_t)back, back_size / 1024);
    } else {
        DBGK("no back buffer, drawing direct\n");
    }

    static struct task comp_task;
    create_task(&comp_task, "compositor", 9, compositor_task);
    compositor_task_ptr = &comp_task;

    DBGK("compositor started (%ldx%ld %d bpp)\n",
         (uint32_t)fb_w, (uint32_t)fb_h, fb_depth);
}

void init_gui(void)
{
    if (!fbdev_is_ready()) {
        DBGK("no framebuffer, GUI disabled\n");
        return;
    }

    struct vbe_mode_info_struct *mode = vbe_get_mode_info();
    if (!mode) {
        DBGK("no VBE mode info, GUI disabled\n");
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

    /* Allocate the back buffer from ZONE_LOW while PMM is unfragmented.
     * pmm_alloc_contiguous guarantees physically contiguous frames, which
     * are also identity-mapped (virtual == physical), giving fast memcpy. */
    uint32_t back_size = (uint32_t)fb_h * fb_pitch;
    uint32_t nframes   = (back_size + PAGE_SIZE - 1) / PAGE_SIZE;
    uint32_t first     = pmm_alloc_contiguous(nframes);
    if (first) {
        for (uint32_t i = 0; i < nframes; i++)
            vmm_map_page(first + i * PAGE_SIZE, first + i * PAGE_SIZE,
                         PTE_PRESENT | PTE_WRITE);
        back = (char *)first;
        back_nframes = nframes;
        DBGK("back buffer reserved at 0x%lx (%ld KB)\n",
             first, back_size / 1024);
    } else {
        back = NULL;
        back_nframes = 0;
        DBGK("back buffer: pmm_alloc_contiguous failed, GUI degraded\n");
    }

    /* Pre-allocate bg_raw and bg_scaled at init time so gui_set_bg() never
     * needs to call vmm_alloc_pages() at runtime.  vmm_alloc_pages is safe
     * here because init_gui() is called before any kernel stacks are
     * allocated (kvirt_next = 0xC0000000), so these large allocations land
     * well below any stack.
     *
     * bg_raw  — sized for the maximum input image (BG_RAW_MAX_W × BG_RAW_MAX_H).
     * bg_scaled — sized for the screen (fb_w × fb_h), the maximum output size. */
    uint32_t raw_size    = BG_RAW_MAX_W * BG_RAW_MAX_H * sizeof(uint32_t);
    uint32_t raw_frames  = (raw_size + PAGE_SIZE - 1) / PAGE_SIZE;
    uint32_t scl_size    = (uint32_t)fb_w * (uint32_t)fb_h * sizeof(uint32_t);
    uint32_t scl_frames  = (scl_size + PAGE_SIZE - 1) / PAGE_SIZE;

    bg_raw = (uint32_t *)vmm_alloc_pages(raw_frames);
    bg_raw_nframes = bg_raw ? raw_frames : 0;

    bg_scaled = (uint32_t *)vmm_alloc_pages(scl_frames);
    bg_scaled_nframes = bg_scaled ? scl_frames : 0;

    memset(win_pool, 0, sizeof(win_pool));
    win_list = NULL;
    next_id  = 1;

    /* Register cleanup hook so task_kill destroys our windows automatically */
    task_death_hook = gui_destroy_windows_for_pid;

    DBGK("subsystem ready (%ldx%ld %d bpp)\n",
         (uint32_t)fb_w, (uint32_t)fb_h, fb_depth);
}

/* ── /dev/x device ───────────────────────────────────────────────────────── */

/* read_op: return current mouse state as struct x_mouse_state */
static uint32_t x_read_op(uint32_t offset, uint32_t len, void *buf)
{
    (void)offset;
    if (len < sizeof(struct x_mouse_state)) return 0;
    struct x_mouse_state *ms = (struct x_mouse_state *)buf;
    ms->x       = mouse_x;
    ms->y       = mouse_y;
    ms->buttons = (uint32_t)mouse_buttons;
    return sizeof(struct x_mouse_state);
}

static int32_t x_control_op(struct vfs_node *node, uint32_t request, void *arg)
{
    (void)node;
    switch (request) {
    case X_WIN_CREATE: {
        struct x_win_create *r = (struct x_win_create *)arg;
        if (!r) return -1;
        if (!r->title || (uint32_t)r->title < USER_SPACE_START
                      || (uint32_t)r->title >= USER_SPACE_END) return -1;
        return gui_window_create(r->x, r->y, r->w, r->h, r->title);
    }
    case X_WIN_DESTROY: {
        struct x_win_destroy *r = (struct x_win_destroy *)arg;
        if (!r) return -1;
        gui_window_destroy(r->id);
        return 0;
    }
    case X_WIN_FILL_RECT: {
        struct x_win_rect *r = (struct x_win_rect *)arg;
        if (!r) return -1;
        gui_window_fill_rect(r->id, r->x, r->y, r->w, r->h, r->color);
        return 0;
    }
    case X_WIN_DRAW_RECT: {
        struct x_win_rect *r = (struct x_win_rect *)arg;
        if (!r) return -1;
        gui_window_draw_rect(r->id, r->x, r->y, r->w, r->h, r->color);
        return 0;
    }
    case X_WIN_DRAW_TEXT: {
        struct x_win_text *r = (struct x_win_text *)arg;
        if (!r || !r->str) return -1;
        if ((uint32_t)r->str < USER_SPACE_START
         || (uint32_t)r->str >= USER_SPACE_END) return -1;
        gui_window_draw_text(r->id, r->x, r->y, r->str, r->fgcolor, r->bgcolor);
        return 0;
    }
    case X_WIN_FLIP: {
        struct x_win_flip *r = (struct x_win_flip *)arg;
        if (!r) return -1;
        gui_window_flip(r->id);
        return 0;
    }
    case X_WIN_POLL_EVENT: {
        struct x_win_poll_event *r = (struct x_win_poll_event *)arg;
        if (!r) return -1;
        return gui_window_poll_event(r->id, (struct gui_event *)r->ev);
    }
    case X_WIN_GET_SIZE: {
        struct x_win_get_size *r = (struct x_win_get_size *)arg;
        if (!r) return -1;
        return gui_window_get_size(r->id, &r->w, &r->h);
    }
    case X_SET_BG: {
        struct x_set_bg *r = (struct x_set_bg *)arg;
        if (!r || !r->pixels || r->w == 0 || r->h == 0) return -1;
        if ((uint32_t)r->pixels < USER_SPACE_START
         || (uint32_t)r->pixels >= USER_SPACE_END) return -1;
        gui_set_bg(r->pixels, r->w, r->h);
        return 0;
    }
    case X_SET_DESKTOP_COLOR: {
        struct x_set_desktop_color *r = (struct x_set_desktop_color *)arg;
        if (!r) return -1;
        gui_set_desktop_color(r->r, r->g, r->b);
        return 0;
    }
    default:
        return -1;
    }
}

void init_dev_x(void)
{
    if (!vfs_register_dev("x", 110, x_read_op, NULL, x_control_op))
        panic("init_dev_x: failed to register /dev/x");
    printk("devfs: /dev/x registered\n");
}
