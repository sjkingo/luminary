#include "cpu/x86.h"
#include "drivers/vga.h"

#define DEFAULT_COLOR (make_color(COLOR_LIGHT_GREY, COLOR_BLACK))

static struct vga_state vid;

static void move_cursor(int x, int y)
{
    int i = vga_index(x, y);
    outb(CRT_PORT, 0xE);
    outb(CRT_PORT+1, (i >> 8));
    outb(CRT_PORT, 0xF);
    outb(CRT_PORT+1, (i & 0xFF));
    vid.cur_x = x;
    vid.cur_y = y;
}

static void putchar_at(int c, int x, int y)
{
    int i = vga_index(x, y);
    vid.buffer[i] = vga_tuple((char)c, vid.def_color);
}

static void scroll_screen(void)
{
    /* move each cell up one row */
    for (unsigned short i = VGA_WIDTH; i < (VGA_HEIGHT * VGA_WIDTH); i++) {
        vid.buffer[i - VGA_WIDTH] = vid.buffer[i];
    }

    /* clear the bottom row */
    for (unsigned short x = 0; x < VGA_WIDTH; x++) {
        putchar_at(' ', x, VGA_HEIGHT-1);
    }

    move_cursor(vid.cur_x, VGA_HEIGHT-1);
}

void put_newline(void)
{
    if (vid.cur_y + 1 >= VGA_HEIGHT)
        scroll_screen();
    vid.cur_x = 0;
    vid.cur_y++;
    move_cursor(vid.cur_x, vid.cur_y);
}

void clear_screen(void)
{
    for (int y = 0; y < VGA_HEIGHT; y++) {
        for (int x = 0; x < VGA_WIDTH; x++) {
            int i = (y * VGA_WIDTH) + x;
            vid.buffer[i] = vga_tuple(' ', vid.def_color);
        }
    }
    move_cursor(0, 0);
}

void putchar(int c)
{
    if (c == '\n') {
        put_newline();
        return;
    }
    if (c == '\b') {
        if (vid.cur_x > 0) {
            move_cursor(vid.cur_x - 1, vid.cur_y);
        }
        return;
    }
    putchar_at(c, vid.cur_x, vid.cur_y);
    move_cursor(vid.cur_x+1, vid.cur_y);
}

void init_vga(void)
{
    vid.buffer = (unsigned short *)0xB8000;
    reset_color();
    vid.cur_x = 0;
    vid.cur_y = 0;
    clear_screen();
}

void set_color(enum vga_color fg, enum vga_color bg)
{
    vid.def_color = make_color(fg, bg);
}

void reset_color(void)
{
    vid.def_color = DEFAULT_COLOR;
}
