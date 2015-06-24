#pragma once

/* Color codes for the VGA memory */
enum vga_color {
    COLOR_BLACK = 0,
    COLOR_BLUE = 1,
    COLOR_GREEN = 2,
    COLOR_CYAN = 3,
    COLOR_RED = 4,
    COLOR_MAGENTA = 5,
    COLOR_BROWN = 6,
    COLOR_LIGHT_GREY = 7,
    COLOR_DARK_GREY = 8,
    COLOR_LIGHT_BLUE = 9,
    COLOR_LIGHT_GREEN = 10,
    COLOR_LIGHT_CYAN = 11,
    COLOR_LIGHT_RED = 12,
    COLOR_LIGHT_MAGENTA = 13,
    COLOR_LIGHT_BROWN = 14,
    COLOR_WHITE = 15,
};

/* The current state of the VGA hardware. Will be init'd by init_video() */
struct vga_state {
    unsigned short *buffer;
    unsigned char def_color;
    int cur_x;
    int cur_y;
};

/* Basic VGA console */
static const int VGA_WIDTH = 80;
static const int VGA_HEIGHT = 24;

/* Some helpful routines that we use a lot */
#define vga_index(x, y) ((y * VGA_WIDTH) + x)
#define make_color(fg, bg) (fg | bg << 4)

/* VGA control register port. Index this port and read from CRT_PORT+1 */
#define CRT_PORT 0x3D4
 
/* Make a 16-bit value (char, color) for storing in VGA buffer */
static inline unsigned short vga_tuple(char c, unsigned char color)
{
    unsigned short c_left = c;
    unsigned short color_right = color;
    return c_left | color_right << 8;
}

/* Call before any attempts to access VGA memory are made */
void init_vga(void);

/* Put a character to the screen */
void putchar(int c);

/* Writes the given string to the status line at the bottom
 * of the screen. See also printsl() in printk.c.
 */
void write_statusline(char *str);

/* Set the color of future putchar() calls to the given color.
 * Reset by calling reset_color()
 */
void set_color(enum vga_color fg, enum vga_color bg);

/* Resets any future putchar() calls to use the default colors. */
void reset_color(void);
