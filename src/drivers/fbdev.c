#include <string.h>
#include "drivers/vbe.h"
#include "drivers/fbdev.h"
#include "kernel/kernel.h"
#include "fonts/ctrld16r.h"

#define MODULE "fbdev: "

/* Solarized Dark palette */
#define SOL_BASE03  rgb( 30,  30,  50)   /* background */
#define SOL_BASE0   rgb(147, 161, 161)   /* foreground: base1 (emphasized content) */

struct fbdev_console {
    char *framebuffer;
    uint32_t height;
    uint32_t width;
    uint8_t depth;
    uint16_t pitch;
    uint32_t cols;
    uint32_t rows;
    uint32_t cur_col;

    /* Scrollback: ring buffer of FBDEV_HISTORY_ROWS rows */
    char cells[FBDEV_MAX_COLS * FBDEV_HISTORY_ROWS];
    uint32_t ring_start;  /* index of oldest row in ring (0..HISTORY_ROWS-1) */
    uint32_t ring_used;   /* number of rows actually stored (0..HISTORY_ROWS) */

    /* View: 0 = show live (most recent rows), N = scrolled N rows back */
    uint32_t view_offset;
};

static struct fbdev_console console;
static bool fbdev_ready = false;

bool fbdev_is_ready(void)
{
    return fbdev_ready;
}

static void putpixel(uint32_t x, uint32_t y, uint32_t color)
{
    if (x >= console.width || y >= console.height)
        return;
    unsigned where = (x * (console.depth / 8)) + (y * console.pitch);
    console.framebuffer[where] = color & 255;              // blue
    console.framebuffer[where + 1] = (color >> 8) & 255;   // green
    console.framebuffer[where + 2] = (color >> 16) & 255;  // red
}

static void clear_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    if (x >= console.width || y >= console.height) return;
    if (x + w > console.width)  w = console.width - x;
    if (y + h > console.height) h = console.height - y;

    uint32_t color = SOL_BASE03;
    uint8_t b0 = color & 0xFF;
    uint8_t b1 = (color >> 8) & 0xFF;
    uint8_t b2 = (color >> 16) & 0xFF;
    uint32_t bpp = console.depth / 8;
    for (uint32_t row = y; row < y + h; row++) {
        unsigned base = (x * bpp) + (row * console.pitch);
        for (uint32_t col = 0; col < w; col++) {
            unsigned where = base + col * bpp;
            console.framebuffer[where]     = b0;
            console.framebuffer[where + 1] = b1;
            console.framebuffer[where + 2] = b2;
        }
    }
}

static void drawglyph(const uint8_t *glyph, uint32_t x, uint32_t y, uint32_t fgcolor)
{
    uint32_t b = 0;
    for (uint32_t j = 0; j < CONSOLE_FONT_HEIGHT; j++) {
        for (uint32_t i = 0; i < CONSOLE_FONT_WIDTH; i++) {
            uint32_t val = (*glyph << b) & 0x80;
            if (val)
                putpixel(x+i, y+j, fgcolor);
            b++;
            if (b == 8) {
                glyph++;
                b = 0;
            }
        }
    }
}

_Static_assert(sizeof(fontdata) >= 256 * CONSOLE_FONT_WIDTH * CONSOLE_FONT_HEIGHT / 8,
        "fontdata too small for 256 glyphs");

static void drawchar(uint8_t c, uint32_t x, uint32_t y, uint32_t fgcolor)
{
    uint32_t g = ((uint32_t)c) * CONSOLE_FONT_WIDTH * CONSOLE_FONT_HEIGHT / 8;
    drawglyph(&fontdata[g], x, y, fgcolor);
}

/* Return a pointer to the cells for ring row index `ring_row` (0..HISTORY_ROWS-1) */
static char *ring_row_ptr(uint32_t ring_row)
{
    return &console.cells[ring_row * FBDEV_MAX_COLS];
}

/* Convert a logical row (0 = oldest kept, ring_used-1 = newest) to ring index */
static uint32_t logical_to_ring(uint32_t logical)
{
    return (console.ring_start + logical) % FBDEV_HISTORY_ROWS;
}

/* Draw a solid block cursor at the current live cursor position */
static void draw_cursor(void)
{
    if (console.ring_used == 0) return;
    if (console.cur_col >= console.cols) return;
    uint32_t screen_row = (console.ring_used <= console.rows)
                          ? (console.ring_used - 1)
                          : (console.rows - 1);
    uint32_t cx = console.cur_col * CONSOLE_FONT_WIDTH;
    uint32_t cy = screen_row * CONSOLE_FONT_HEIGHT;

    for (uint32_t row = cy; row < cy + CONSOLE_FONT_HEIGHT; row++)
        for (uint32_t col = cx; col < cx + CONSOLE_FONT_WIDTH; col++)
            putpixel(col, row, SOL_BASE0);

    uint32_t live_ring = logical_to_ring(console.ring_used - 1);
    char c = ring_row_ptr(live_ring)[console.cur_col];
    if (c && c != ' ')
        drawchar((uint8_t)c, cx, cy, SOL_BASE03);
}

/* Erase the cursor at the current live cursor position (redraw cell normally) */
static void erase_cursor(void)
{
    if (console.ring_used == 0) return;
    if (console.cur_col >= console.cols) return;
    uint32_t screen_row = (console.ring_used <= console.rows)
                          ? (console.ring_used - 1)
                          : (console.rows - 1);
    uint32_t cx = console.cur_col * CONSOLE_FONT_WIDTH;
    uint32_t cy = screen_row * CONSOLE_FONT_HEIGHT;

    clear_rect(cx, cy, CONSOLE_FONT_WIDTH, CONSOLE_FONT_HEIGHT);
    uint32_t live_ring = logical_to_ring(console.ring_used - 1);
    char c = ring_row_ptr(live_ring)[console.cur_col];
    if (c && c != ' ')
        drawchar((uint8_t)c, cx, cy, SOL_BASE0);
}

/* Redraw the entire screen from scrollback history */
static void redraw_all(void)
{
    uint32_t fgcolor = SOL_BASE0;

    /* Clear the whole framebuffer to background color */
    clear_rect(0, 0, console.width, console.height);

    /* Determine first logical row to show.
     * Logical rows: 0 = oldest, ring_used-1 = newest live row.
     * view_offset=0 → show last `rows` logical rows.
     * view_offset=N → show rows shifted N back. */
    uint32_t total = console.ring_used;
    uint32_t visible = console.rows;

    /* first_logical = max(0, total - visible - view_offset) */
    uint32_t skip = visible + console.view_offset;
    uint32_t first_logical = (total > skip) ? (total - skip) : 0;

    uint32_t screen_row = 0;
    for (uint32_t log = first_logical; log < total && screen_row < visible; log++, screen_row++) {
        uint32_t ring_idx = logical_to_ring(log);
        char *row = ring_row_ptr(ring_idx);
        for (uint32_t col = 0; col < console.cols; col++) {
            char c = row[col];
            if (c && c != ' ') {
                drawchar((uint8_t)c,
                         col * CONSOLE_FONT_WIDTH,
                         screen_row * CONSOLE_FONT_HEIGHT,
                         fgcolor);
            }
        }
    }
}

/* Advance the live write position by one row, growing the ring if needed */
static void advance_live_row(void)
{
    if (console.ring_used < FBDEV_HISTORY_ROWS) {
        /* Still growing: just append */
        console.ring_used++;
    } else {
        /* Ring full: overwrite oldest row by advancing ring_start */
        console.ring_start = (console.ring_start + 1) % FBDEV_HISTORY_ROWS;
    }

    /* Clear the new live row */
    uint32_t new_ring = logical_to_ring(console.ring_used - 1);
    memset(ring_row_ptr(new_ring), 0, FBDEV_MAX_COLS);
}

static void scroll(void)
{
    advance_live_row();

    /* Fast path: shift framebuffer up by one character row and clear the bottom.
     * Avoids a full redraw_all() on every newline — O(screen) instead of O(screen * rows). */
    uint32_t row_bytes = (uint32_t)CONSOLE_FONT_HEIGHT * console.pitch;
    uint32_t total_rows = console.rows;
    char *fb = console.framebuffer;

    /* Copy rows [1..rows-1] up to [0..rows-2]. Destination is before source
     * so memcpy is safe (no backwards overlap). */
    memcpy(fb, fb + row_bytes, row_bytes * (total_rows - 1));

    /* Clear the bottom row */
    uint32_t bottom_y = (total_rows - 1) * CONSOLE_FONT_HEIGHT;
    clear_rect(0, bottom_y, console.width, CONSOLE_FONT_HEIGHT);

    /* Repaint the bottom row from the new live ring row */
    uint32_t live_ring = logical_to_ring(console.ring_used - 1);
    char *row = ring_row_ptr(live_ring);
    for (uint32_t col = 0; col < console.cols; col++) {
        char c = row[col];
        if (c && c != ' ')
            drawchar((uint8_t)c,
                     col * CONSOLE_FONT_WIDTH,
                     bottom_y,
                     SOL_BASE0);
    }
}

void writechar_fb(char c)
{
    if (!fbdev_ready)
        return;

    /* Any new output snaps view back to live */
    if (console.view_offset != 0) {
        console.view_offset = 0;
    }

    erase_cursor();

    if (c == '\n') {
        console.cur_col = 0;
        if (console.ring_used < console.rows) {
            /* Screen not full yet: just add a new row without scrolling */
            console.ring_used++;
            uint32_t new_ring = logical_to_ring(console.ring_used - 1);
            memset(ring_row_ptr(new_ring), 0, FBDEV_MAX_COLS);
        } else {
            scroll();
        }
    } else if (c == '\r') {
        console.cur_col = 0;
    } else if (c == '\b') {
        if (console.cur_col > 0) {
            console.cur_col--;
            uint32_t ring_idx = logical_to_ring(console.ring_used - 1);
            ring_row_ptr(ring_idx)[console.cur_col] = ' ';
            uint32_t screen_row = (console.ring_used <= console.rows)
                                  ? (console.ring_used - 1)
                                  : (console.rows - 1);
            clear_rect(console.cur_col * CONSOLE_FONT_WIDTH,
                        screen_row * CONSOLE_FONT_HEIGHT,
                        CONSOLE_FONT_WIDTH, CONSOLE_FONT_HEIGHT);
        }
    } else if (c == '\t') {
        console.cur_col = (console.cur_col + 4) & ~3;
        if (console.cur_col >= console.cols) {
            console.cur_col = 0;
            if (console.ring_used < console.rows) {
                console.ring_used++;
                uint32_t new_ring = logical_to_ring(console.ring_used - 1);
                memset(ring_row_ptr(new_ring), 0, FBDEV_MAX_COLS);
            } else {
                scroll();
            }
        }
    } else {
        /* Wrap if we hit the right edge */
        if (console.cur_col >= console.cols) {
            console.cur_col = 0;
            if (console.ring_used < console.rows) {
                console.ring_used++;
                uint32_t new_ring = logical_to_ring(console.ring_used - 1);
                memset(ring_row_ptr(new_ring), 0, FBDEV_MAX_COLS);
            } else {
                scroll();
            }
        }

        /* Write to the current live ring row */
        uint32_t live_ring = logical_to_ring(console.ring_used - 1);
        ring_row_ptr(live_ring)[console.cur_col] = c;

        /* Screen row: before screen fills, = ring_used-1; after, always bottom row */
        uint32_t screen_row = (console.ring_used <= console.rows)
                              ? (console.ring_used - 1)
                              : (console.rows - 1);
        drawchar((uint8_t)c,
                 console.cur_col * CONSOLE_FONT_WIDTH,
                 screen_row * CONSOLE_FONT_HEIGHT,
                 SOL_BASE0);
        console.cur_col++;
    }

    draw_cursor();
}

void writestr_fb(char *str)
{
    while (*str)
        writechar_fb(*str++);
}

void writeline_fb(uint32_t row, char *str, uint32_t fgcolor,
        uint32_t bgcolor __attribute__((unused)))
{
    if (!fbdev_ready || row >= console.rows)
        return;

    /* Clear the row on screen */
    clear_rect(0, row * CONSOLE_FONT_HEIGHT,
            console.width, CONSOLE_FONT_HEIGHT);

    /* Draw each character */
    uint32_t col = 0;
    while (*str && col < console.cols) {
        drawchar(*str++, col * CONSOLE_FONT_WIDTH,
                row * CONSOLE_FONT_HEIGHT, fgcolor);
        col++;
    }
}

void fbdev_redraw(void)
{
    if (!fbdev_ready)
        return;
    redraw_all();
}

void fbdev_scroll_up(void)
{
    if (!fbdev_ready)
        return;
    /* Can scroll back at most ring_used - rows rows */
    uint32_t max_offset = (console.ring_used > console.rows)
                          ? (console.ring_used - console.rows) : 0;
    if (console.view_offset + FBDEV_SCROLL_STEP <= max_offset)
        console.view_offset += FBDEV_SCROLL_STEP;
    else
        console.view_offset = max_offset;
    redraw_all();
}

void fbdev_scroll_down(void)
{
    if (!fbdev_ready)
        return;
    if (console.view_offset >= FBDEV_SCROLL_STEP)
        console.view_offset -= FBDEV_SCROLL_STEP;
    else
        console.view_offset = 0;
    redraw_all();
}

void init_fbdev(char *fbaddr, uint32_t width, uint32_t height, uint8_t depth,
        uint16_t pitch)
{
    console.framebuffer = fbaddr;
    console.height = height;
    console.width = width;
    console.depth = depth;
    console.pitch = pitch;
    console.cols = width / CONSOLE_FONT_WIDTH;
    console.rows = height / CONSOLE_FONT_HEIGHT;
    console.cur_col = 0;
    console.ring_start = 0;
    console.ring_used = 1;  /* start with one empty live row */
    console.view_offset = 0;

    /* Cap to max buffer size */
    if (console.cols > FBDEV_MAX_COLS)
        console.cols = FBDEV_MAX_COLS;
    if (console.rows > FBDEV_MAX_ROWS)
        console.rows = FBDEV_MAX_ROWS;

    /* Refuse to operate on a display too small for even one character */
    if (console.cols == 0 || console.rows == 0) {
        printk(MODULE "display too small for console (%ldx%ld)\n", width, height);
        return;
    }

    /* Clear the history buffer */
    memset(console.cells, 0, sizeof(console.cells));

    fbdev_ready = true;

    /* Paint the background color immediately */
    clear_rect(0, 0, console.width, console.height);

    printk(MODULE "framebuffer console %ldx%ld (%ld cols x %ld rows, %d row history)\n",
            width, height, console.cols, console.rows, FBDEV_HISTORY_ROWS);
}
