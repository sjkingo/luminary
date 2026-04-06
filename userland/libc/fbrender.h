/* fbrender.h — inline framebuffer rendering helpers for userland.
 *
 * Include this header in any userland program that needs to draw characters
 * directly to the VBE framebuffer obtained via /dev/fb0.
 *
 * Usage:
 *   #include "libc/fbrender.h"      // pulls in font data too
 *   fbrender_init(addr, w, h, depth, pitch);
 *   fbrender_putchar(ch, col, row);
 *   fbrender_clear_rect(x, y, w, h);
 *
 * Performance: optimised for 32bpp VBE (QEMU default). clear_rect and
 * putchar use 32-bit pixel writes to halve MMIO traffic vs 3-byte stores.
 * render_row renders a full text row by drawing all glyphs into a local
 * scanline buffer, then flushing each scanline with a single memcpy to the
 * framebuffer. This turns O(cols * font_h * font_w) random MMIO writes into
 * O(font_h) sequential memcpy calls — ~10× faster on QEMU.
 */
#pragma once

#include "libc/string.h"
#include "fonts/ctrld16r.h"   /* CONSOLE_FONT_WIDTH, CONSOLE_FONT_HEIGHT, fontdata[] */

/* Pixel byte order is BGR (b0=Blue, b1=Green, b2=Red).
 * Encode as (R<<16)|(G<<8)|B so writes land correctly for both 24bpp and
 * 32bpp (4th byte is ignored/padding in 32bpp mode). */
#define FBRENDER_COL_BG  ((unsigned int)(( 30u << 16) | ( 30u << 8) |  50u))   /* rgb(30,30,50) */
#define FBRENDER_COL_FG  ((unsigned int)((147u << 16) | (161u << 8) | 161u))   /* rgb(147,161,161) */

static unsigned char *fbrender_fb;     /* hardware framebuffer (MMIO) */
static unsigned char *fbrender_shadow; /* RAM shadow; NULL = write direct to hw */
static unsigned int   fbrender_w;
static unsigned int   fbrender_h;
static unsigned int   fbrender_pitch;
static unsigned int   fbrender_bpp;

/* fbrender_init: initialise with hardware framebuffer.
 * Call fbrender_set_shadow(buf) separately to enable shadow-buffer mode. */
static inline void fbrender_init(unsigned char *fb, unsigned int w,
        unsigned int h, unsigned char depth, unsigned int pitch)
{
    fbrender_fb     = fb;
    fbrender_shadow = 0;
    fbrender_w      = w;
    fbrender_h      = h;
    fbrender_pitch  = pitch;
    fbrender_bpp    = (unsigned int)depth / 8u;
}

/* Set a RAM shadow buffer (must be pitch*h bytes).
 * When set, all rendering goes to shadow; call fbrender_present() to blit. */
static inline void fbrender_set_shadow(unsigned char *shadow)
{
    fbrender_shadow = shadow;
}

/* Blit entire shadow to hardware framebuffer in one memcpy. */
static inline void fbrender_present(void)
{
    if (!fbrender_shadow) return;
    memcpy(fbrender_fb, fbrender_shadow, fbrender_pitch * fbrender_h);
}

/* Blit only the scanlines covering text rows first_row..last_row (inclusive). */
static inline void fbrender_present_rows(int first_row, int last_row)
{
    unsigned int y0, y1, nbytes;
    if (!fbrender_shadow) return;
    y0 = (unsigned int)first_row * CONSOLE_FONT_HEIGHT;
    y1 = (unsigned int)(last_row + 1) * CONSOLE_FONT_HEIGHT;
    if (y1 > fbrender_h) y1 = fbrender_h;
    nbytes = (y1 - y0) * fbrender_pitch;
    memcpy(fbrender_fb + y0 * fbrender_pitch,
           fbrender_shadow + y0 * fbrender_pitch,
           nbytes);
}

/* Return the active render target (shadow if set, hw otherwise). */
static inline unsigned char *fbrender_target(void)
{
    return fbrender_shadow ? fbrender_shadow : fbrender_fb;
}

/* Write one pixel. Used only for the cursor overlay (a few dozen pixels). */
static inline void fbrender_putpixel(unsigned int x, unsigned int y,
        unsigned int color)
{
    unsigned int where;
    unsigned char *tgt;
    if (x >= fbrender_w || y >= fbrender_h) return;
    where = x * fbrender_bpp + y * fbrender_pitch;
    tgt = fbrender_target();
    tgt[where]     = (unsigned char)(color & 0xFF);
    tgt[where + 1] = (unsigned char)((color >> 8) & 0xFF);
    tgt[where + 2] = (unsigned char)((color >> 16) & 0xFF);
}

/* Fill a rectangle with the background colour.
 * For 32bpp, writes 4 bytes per pixel using a uint32_t store to minimise
 * MMIO transactions. Falls back to byte writes for 24bpp. */
static inline void fbrender_clear_rect(unsigned int x, unsigned int y,
        unsigned int w, unsigned int h)
{
    unsigned int r;
    unsigned int c;

    if (x >= fbrender_w || y >= fbrender_h) return;
    if (x + w > fbrender_w) w = fbrender_w - x;
    if (y + h > fbrender_h) h = fbrender_h - y;

    {
        unsigned char *tgt = fbrender_target();
        if (fbrender_bpp == 4) {
            unsigned int pix32 = FBRENDER_COL_BG;
            for (r = y; r < y + h; r++) {
                unsigned int *row = (unsigned int *)(void *)(tgt + r * fbrender_pitch + x * 4u);
                for (c = 0; c < w; c++)
                    row[c] = pix32;
            }
        } else {
            unsigned char b0 = (unsigned char)(FBRENDER_COL_BG & 0xFF);
            unsigned char b1 = (unsigned char)((FBRENDER_COL_BG >> 8) & 0xFF);
            unsigned char b2 = (unsigned char)((FBRENDER_COL_BG >> 16) & 0xFF);
            for (r = y; r < y + h; r++) {
                unsigned char *base = tgt + r * fbrender_pitch + x * fbrender_bpp;
                for (c = 0; c < w; c++) {
                    base[c * 3]     = b0;
                    base[c * 3 + 1] = b1;
                    base[c * 3 + 2] = b2;
                }
            }
        }
    }
}

/* Render one character cell directly into the framebuffer.
 * Prefer fbrender_render_row for full-row updates — it is much faster. */
static inline void fbrender_putchar(unsigned char ch, unsigned int col,
        unsigned int row)
{
    unsigned int glyph_off = (unsigned int)ch * CONSOLE_FONT_WIDTH * CONSOLE_FONT_HEIGHT / 8;
    const unsigned char *glyph = &fontdata[glyph_off];
    unsigned int px = col * CONSOLE_FONT_WIDTH;
    unsigned int py = row * CONSOLE_FONT_HEIGHT;
    unsigned int j;
    unsigned int i;
    unsigned int b = 0;

    fbrender_clear_rect(px, py, CONSOLE_FONT_WIDTH, CONSOLE_FONT_HEIGHT);
    for (j = 0; j < CONSOLE_FONT_HEIGHT; j++) {
        for (i = 0; i < CONSOLE_FONT_WIDTH; i++) {
            if ((*glyph << b) & 0x80)
                fbrender_putpixel(px + i, py + j, FBRENDER_COL_FG);
            if (++b == 8) { glyph++; b = 0; }
        }
    }
}

/* Render a full text row by compositing all glyphs into a local scanline
 * buffer, then copying each scanline to the framebuffer with a single
 * memcpy. This drastically reduces MMIO writes on QEMU.
 *
 * scanline_buf holds one horizontal strip: cols * FONT_W pixels.
 * Max terminal width is TERMEMU_MAX_COLS = 256 (see termemu.h).
 * 256 * 8px * 4B = 8192 bytes on the stack — acceptable. */
static inline void fbrender_render_row(struct termemu *t, int screen_row)
{
    char *line = termemu_get_visible_row(t, screen_row);
    unsigned int cols = (unsigned int)t->cols;
    unsigned int row_w = cols * CONSOLE_FONT_WIDTH;
    unsigned int scan_bytes;
    unsigned int py;
    unsigned int j;
    unsigned int c;
    /* Stack buffer for one scanline of the text row (32bpp max). */
    unsigned int scanline[256 * CONSOLE_FONT_WIDTH];

    py = (unsigned int)screen_row * CONSOLE_FONT_HEIGHT;

    if (fbrender_bpp == 4) {
        scan_bytes = row_w * 4u;
        for (j = 0; j < CONSOLE_FONT_HEIGHT; j++) {
            /* Fill with background */
            for (c = 0; c < row_w; c++)
                scanline[c] = FBRENDER_COL_BG;
            /* Stamp foreground pixels for each glyph on this scanline */
            for (c = 0; c < cols; c++) {
                unsigned char ch = (unsigned char)line[c];
                unsigned int glyph_off;
                const unsigned char *glyph;
                unsigned int i;
                unsigned int b;
                unsigned int row_in_glyph;
                if (ch < 32) ch = (unsigned char)' ';
                glyph_off = (unsigned int)ch * CONSOLE_FONT_WIDTH * CONSOLE_FONT_HEIGHT / 8;
                row_in_glyph = j * CONSOLE_FONT_WIDTH;
                glyph = &fontdata[glyph_off + row_in_glyph / 8];
                b = row_in_glyph % 8;
                for (i = 0; i < CONSOLE_FONT_WIDTH; i++) {
                    if ((*glyph << b) & 0x80)
                        scanline[c * CONSOLE_FONT_WIDTH + i] = FBRENDER_COL_FG;
                    if (++b == 8) { glyph++; b = 0; }
                }
            }
            /* Flush scanline to render target */
            memcpy(fbrender_target() + (py + j) * fbrender_pitch,
                   scanline, scan_bytes);
        }
    } else {
        /* 24bpp fallback: render char-by-char (no scanline buffer) */
        for (c = 0; c < cols; c++) {
            unsigned char ch = (unsigned char)line[c];
            if (ch < 32) ch = (unsigned char)' ';
            fbrender_putchar(ch, c, (unsigned int)screen_row);
        }
    }
}

static inline void fbrender_render_cursor(struct termemu *t)
{
    unsigned int cx;
    unsigned int cy;
    unsigned int px;
    unsigned int py;
    unsigned int j;
    unsigned int i;

    if (t->scroll_offset != 0) return;
    if (t->cur_col >= t->cols) return;

    cx = (unsigned int)t->cur_col;
    cy = (unsigned int)t->cur_row;
    px = cx * CONSOLE_FONT_WIDTH;
    py = cy * CONSOLE_FONT_HEIGHT;

    for (j = py; j < py + CONSOLE_FONT_HEIGHT; j++)
        for (i = px; i < px + CONSOLE_FONT_WIDTH; i++)
            fbrender_putpixel(i, j, FBRENDER_COL_FG);

    /* Draw character in reverse if non-space */
    {
        char *live = termemu_get_live_row(t, cy);
        unsigned char ch = live[cx];
        if (ch >= 32 && ch != ' ') {
            unsigned int glyph_off = (unsigned int)ch * CONSOLE_FONT_WIDTH * CONSOLE_FONT_HEIGHT / 8;
            const unsigned char *glyph = &fontdata[glyph_off];
            unsigned int b = 0;
            unsigned int jj;
            unsigned int ii;
            for (jj = 0; jj < CONSOLE_FONT_HEIGHT; jj++) {
                for (ii = 0; ii < CONSOLE_FONT_WIDTH; ii++) {
                    if ((*glyph << b) & 0x80)
                        fbrender_putpixel(px + ii, py + jj, FBRENDER_COL_BG);
                    if (++b == 8) { glyph++; b = 0; }
                }
            }
        }
    }
}
