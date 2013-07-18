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
}

void put_newline(void)
{
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
    putchar_at(c, vid.cur_x, vid.cur_y);
    move_cursor(vid.cur_x+1, vid.cur_y);
}

void init_vga(void)
{
    vid.buffer = (unsigned short *)0xB8000;
    vid.def_color = make_color(COLOR_LIGHT_GREY, COLOR_BLACK);
    vid.cur_x = 0;
    vid.cur_y = 0;
    clear_screen();
    write_statusline("");
}

void write_statusline(char *str)
{
    /* change the colour of the status line */
    unsigned char def_color = vid.def_color;
    vid.def_color = make_color(COLOR_WHITE, COLOR_BROWN);

    /* print the string */
    int x = 0;
    while (str[x] != '\0' && x < VGA_WIDTH) {
        putchar_at(str[x], x, VGA_HEIGHT);
        x++;
    }

    /* pad the rest of the line */
    if (x < VGA_WIDTH) {
        while (x < VGA_WIDTH) {
            putchar_at(' ', x, VGA_HEIGHT);
            x++;
        }
    }

    /* restore the default colour */
    vid.def_color = def_color;
}
