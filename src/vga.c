#include "vga.h"
#include "x86.h"

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
    move_cursor(x+1, y);
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
    putchar_at(c, vid.cur_x, vid.cur_y);
}

void init_vga(void)
{
    vid.buffer = (unsigned short *)0xB8000;
    vid.def_color = make_color(COLOR_LIGHT_GREY, COLOR_BLACK);
    vid.cur_x = 0;
    vid.cur_y = 0;
    clear_screen();
}
