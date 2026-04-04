/* PS/2 mouse driver - IRQ12, 3-byte packet parsing, absolute position tracking.
 *
 * PS/2 mouse sends 3-byte packets:
 *   Byte 0: [Y-overflow][X-overflow][Y-sign][X-sign][1][middle][right][left]
 *   Byte 1: X movement (8-bit signed delta, sign bit in byte 0)
 *   Byte 2: Y movement (8-bit signed delta, sign bit in byte 0, Y-axis inverted)
 */

#include "cpu/x86.h"
#include "drivers/mouse.h"
#include "drivers/vbe.h"
#include "kernel/gui.h"

#define MOUSE_DATA_PORT     0x60
#define MOUSE_CMD_PORT      0x64
#define MOUSE_STATUS_PORT   0x64

#define MOUSE_BUF_SIZE  64

/* Absolute cursor position */
uint32_t mouse_x = 0;
uint32_t mouse_y = 0;

/* Current button state */
uint8_t mouse_buttons = 0;

/* Screen bounds - set by init_mouse() from VBE info */
static uint32_t screen_w = 1024;
static uint32_t screen_h = 768;

/* 3-byte packet assembly state */
static uint8_t  packet[3];
static int      packet_idx = 0;

/* Decoded event ring buffer */
static struct {
    struct mouse_event data[MOUSE_BUF_SIZE];
    unsigned int head;
    unsigned int tail;
} mouse_buf;

/* Wait for PS/2 controller input buffer to be empty (ready to write) */
static void ps2_wait_write(void)
{
    int timeout = 100000;
    while (timeout-- && (inb(MOUSE_STATUS_PORT) & 0x02))
        ;
}

/* Wait for PS/2 controller output buffer to have data (ready to read) */
static void ps2_wait_read(void)
{
    int timeout = 100000;
    while (timeout-- && !(inb(MOUSE_STATUS_PORT) & 0x01))
        ;
}

static void ps2_write_cmd(uint8_t cmd)
{
    ps2_wait_write();
    outb(MOUSE_CMD_PORT, cmd);
}

static void ps2_write_data(uint8_t data)
{
    ps2_wait_write();
    outb(MOUSE_DATA_PORT, data);
}

static uint8_t ps2_read_data(void)
{
    ps2_wait_read();
    return inb(MOUSE_DATA_PORT);
}

/* Send a byte to the mouse device (via the PS/2 controller aux port) */
static void mouse_write(uint8_t data)
{
    ps2_write_cmd(0xD4);    /* tell controller: next byte goes to mouse */
    ps2_write_data(data);
}

/* Drain any stale bytes sitting in the PS/2 output buffer */
static void ps2_flush(void)
{
    int timeout = 32;
    while (timeout-- && (inb(MOUSE_STATUS_PORT) & 0x01))
        inb(MOUSE_DATA_PORT);
}

void init_mouse(uint32_t w, uint32_t h)
{
    screen_w = w;
    screen_h = h;

    mouse_buf.head = 0;
    mouse_buf.tail = 0;
    packet_idx = 0;

    /* Start cursor in centre of screen */
    mouse_x = screen_w / 2;
    mouse_y = screen_h / 2;

    /* Flush any stale data before touching the controller */
    ps2_flush();

    /* Enable the PS/2 auxiliary (mouse) port */
    ps2_write_cmd(0xA8);

    /* Enable interrupts from mouse (set bit 1 of compaq status byte) */
    ps2_write_cmd(0x20);            /* read compaq status byte */
    uint8_t status = ps2_read_data();
    status |= 0x02;                 /* enable IRQ12 */
    status &= ~0x20;                /* clear "mouse clock disabled" bit */
    ps2_write_cmd(0x60);            /* write compaq status byte */
    ps2_write_data(status);

    /* Flush again before reset (writing compaq status may have caused activity) */
    ps2_flush();

    /* Reset mouse and wait for ACK + self-test result */
    mouse_write(0xFF);
    ps2_read_data();    /* ACK (0xFA) */
    ps2_read_data();    /* 0xAA self-test pass */
    ps2_read_data();    /* 0x00 device ID */
    ps2_flush();        /* consume any extra bytes the reset may have emitted */

    /* Set default settings then bump resolution and sample rate */
    mouse_write(0xF6);  /* set defaults */
    ps2_read_data();    /* ACK */

    mouse_write(0xE8);  /* set resolution */
    ps2_read_data();    /* ACK */
    mouse_write(0x03);  /* 8 counts/mm (highest) */
    ps2_read_data();    /* ACK */

    mouse_write(0xF3);  /* set sample rate */
    ps2_read_data();    /* ACK */
    mouse_write(200);   /* 200 samples/sec */
    ps2_read_data();    /* ACK */

    mouse_write(0xF4);  /* enable data reporting */
    ps2_read_data();    /* ACK */
}

static void mouse_buf_put(struct mouse_event *ev)
{
    unsigned int next = (mouse_buf.head + 1) % MOUSE_BUF_SIZE;
    if (next == mouse_buf.tail)
        return; /* full, drop */
    mouse_buf.data[mouse_buf.head] = *ev;
    mouse_buf.head = next;
}

void mouse_irq_handler(void)
{
    uint8_t byte = inb(MOUSE_DATA_PORT);
    packet[packet_idx++] = byte;

    if (packet_idx < 3)
        return;

    /* We have a full 3-byte packet */
    packet_idx = 0;

    uint8_t flags = packet[0];

    /* Bit 3 must always be set in the flags byte.
     * If not, we're out of sync — scan for a byte with bit 3 set
     * and shift the remaining bytes to realign. */
    if (!(flags & 0x08)) {
        /* Try packet[1] as the new flags byte */
        if (packet[1] & 0x08) {
            packet[0] = packet[1];
            packet[1] = packet[2];
            packet_idx = 2; /* need one more byte */
        } else if (packet[2] & 0x08) {
            packet[0] = packet[2];
            packet_idx = 1; /* need two more bytes */
        }
        /* else discard all three and wait for a fresh start */
        return;
    }

    /* Ignore overflow packets */
    if (flags & 0xC0)
        return;

    /* Decode signed deltas using sign bits from flags byte */
    int16_t dx = (int16_t)packet[1] - ((flags & 0x10) ? 256 : 0);
    int16_t dy = (int16_t)packet[2] - ((flags & 0x20) ? 256 : 0);

    /* Y axis is inverted on PS/2 (positive = up), flip to screen coords */
    dy = -dy;

    /* Clamp raw deltas to sane range before multiplying */
    if (dx >  32) dx =  32;
    if (dx < -32) dx = -32;
    if (dy >  32) dy =  32;
    if (dy < -32) dy = -32;

    /* 2x sensitivity */
    dx = dx * 2;
    dy = dy * 2;

    /* Update absolute position, clamped to screen */
    int32_t nx = (int32_t)mouse_x + dx;
    int32_t ny = (int32_t)mouse_y + dy;
    if (nx < 0) nx = 0;
    if (ny < 0) ny = 0;
    if ((uint32_t)nx >= screen_w) nx = (int32_t)(screen_w - 1);
    if ((uint32_t)ny >= screen_h) ny = (int32_t)(screen_h - 1);
    mouse_x = (uint32_t)nx;
    mouse_y = (uint32_t)ny;

    mouse_buttons = flags & 0x07;

    struct mouse_event ev;
    ev.dx      = dx;
    ev.dy      = dy;
    ev.buttons = mouse_buttons;
    mouse_buf_put(&ev);
    gui_wake();
}

int mouse_read(struct mouse_event *ev)
{
    if (mouse_buf.tail == mouse_buf.head)
        return 0;
    *ev = mouse_buf.data[mouse_buf.tail];
    mouse_buf.tail = (mouse_buf.tail + 1) % MOUSE_BUF_SIZE;
    return 1;
}
