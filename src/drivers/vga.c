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
    for (unsigned short i = 0; i < (VGA_HEIGHT * VGA_WIDTH); i++) {
        unsigned short this = vid.buffer[i];
        vid.buffer[i-VGA_WIDTH] = this;
    }

    /* clear the bottom row */
    for (unsigned short x = 0; x < VGA_WIDTH; x++) {
        putchar_at(' ', x, VGA_HEIGHT-1);
    }

    move_cursor(vid.cur_x, VGA_HEIGHT-2);
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
    write_statusline("");
}

void write_top_right(char *str)
{
    int x = VGA_WIDTH - strlen(str);
    int y = 0;
    unsigned char def_color = vid.def_color;
    vid.def_color = make_color(COLOR_WHITE, COLOR_BLUE);
    for (int i = 0; i < strlen(str); i++) {
        putchar_at(str[i], x++, y);
    }
    vid.def_color = def_color;
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

void set_color(enum vga_color fg, enum vga_color bg)
{
    vid.def_color = make_color(fg, bg);
}

void reset_color(void)
{
    vid.def_color = DEFAULT_COLOR;
}
