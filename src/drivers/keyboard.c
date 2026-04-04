/* PS/2 keyboard driver - IRQ1 handler, scancode translation, ring buffer.
 * Adapted from Ulysses arch/x86/keyboard.c for Luminary conventions.
 */

#include "cpu/x86.h"
#include "drivers/keyboard.h"

#define KB_DATA_PORT 0x60

/* Scancode set 1 - unshifted */
static const char scancode_unshifted[88] = {
       0, 0x1B,  '1',  '2',  '3',  '4',  '5',  '6',    /* 0x00-0x07 */
     '7',  '8',  '9',  '0',  '-',  '=', '\b', ' ',     /* 0x08-0x0F (BS, Tab->space) */
     'q',  'w',  'e',  'r',  't',  'y',  'u',  'i',    /* 0x10-0x17 */
     'o',  'p',  '[',  ']', '\n',    0,  'a',  's',    /* 0x18-0x1F (Enter, LCtrl) */
     'd',  'f',  'g',  'h',  'j',  'k',  'l',  ';',    /* 0x20-0x27 */
    '\'',  '`',    0, '\\',  'z',  'x',  'c',  'v',    /* 0x28-0x2F (LShift) */
     'b',  'n',  'm',  ',',  '.',  '/',    0,  '*',    /* 0x30-0x37 (RShift, KP*) */
       0,  ' ',    0,    0,    0,    0,    0,    0,      /* 0x38-0x3F (LAlt, Space, Caps, F1-F5) */
       0,    0,    0,    0,    0,    0,    0,  '7',      /* 0x40-0x47 (F6-F10, Num, Scroll, KP7) */
     '8',  '9',  '-',  '4',  '5',  '6',  '+',  '1',    /* 0x48-0x4F (KP8-KP+, KP1) */
     '2',  '3',  '0',  '.',    0,    0,    0,    0,      /* 0x50-0x57 (KP2-KP., F11, F12) */
};

/* Scancode set 1 - shifted */
static const char scancode_shifted[88] = {
       0, 0x1B,  '!',  '@',  '#',  '$',  '%',  '^',    /* 0x00-0x07 */
     '&',  '*',  '(',  ')',  '_',  '+', '\b', ' ',     /* 0x08-0x0F */
     'Q',  'W',  'E',  'R',  'T',  'Y',  'U',  'I',    /* 0x10-0x17 */
     'O',  'P',  '{',  '}', '\n',    0,  'A',  'S',    /* 0x18-0x1F */
     'D',  'F',  'G',  'H',  'J',  'K',  'L',  ':',    /* 0x20-0x27 */
     '"',  '~',    0,  '|',  'Z',  'X',  'C',  'V',    /* 0x28-0x2F */
     'B',  'N',  'M',  '<',  '>',  '?',    0,  '*',    /* 0x30-0x37 */
       0,  ' ',    0,    0,    0,    0,    0,    0,      /* 0x38-0x3F */
       0,    0,    0,    0,    0,    0,    0,  '7',      /* 0x40-0x47 */
     '8',  '9',  '-',  '4',  '5',  '6',  '+',  '1',    /* 0x48-0x4F */
     '2',  '3',  '0',  '.',    0,    0,    0,    0,      /* 0x50-0x57 */
};

/* Ring buffer - single producer (IRQ), single consumer (syscall) */
static struct {
    char data[KB_BUFFER_SIZE];
    unsigned int head; /* write index */
    unsigned int tail; /* read index */
    int shift;
    int ctrl;
    int caps;
    int extended; /* set when 0xE0 prefix received */
} kb;

void init_keyboard(void)
{
    kb.head = 0;
    kb.tail = 0;
    kb.shift = 0;
    kb.ctrl  = 0;
    kb.caps = 0;
    kb.extended = 0;
}

static void kb_buf_put(char c)
{
    unsigned int next = (kb.head + 1) % KB_BUFFER_SIZE;
    if (next == kb.tail)
        return; /* buffer full, drop keystroke */
    kb.data[kb.head] = c;
    kb.head = next;
}

void keyboard_irq_handler(void)
{
    unsigned char scancode = inb(KB_DATA_PORT);

    /* Extended scancode prefix */
    if (scancode == 0xE0) {
        kb.extended = 1;
        return;
    }

    if (kb.extended) {
        kb.extended = 0;
        /* Page Up (0xE0 0x49) and Page Down (0xE0 0x51) - make codes only */
        if (scancode == 0x49) { kb_buf_put(KEY_PGUP); return; }
        if (scancode == 0x51) { kb_buf_put(KEY_PGDN); return; }
        /* Right Ctrl make/break (0xE0 0x1D / 0xE0 0x9D) */
        if (scancode == 0x1D) { kb.ctrl = 1; return; }
        if (scancode == 0x9D) { kb.ctrl = 0; return; }
        /* Ignore all other extended keys */
        return;
    }

    /* Shift make/break */
    if (scancode == 0x2A || scancode == 0x36) {
        kb.shift = 1;
        return;
    }
    if (scancode == 0xAA || scancode == 0xB6) {
        kb.shift = 0;
        return;
    }

    /* Ctrl make/break (left ctrl = 0x1D make, 0x9D break).
     * Must be checked BEFORE the generic break-code filter below. */
    if (scancode == 0x1D) { kb.ctrl = 1; return; }
    if (scancode == 0x9D) { kb.ctrl = 0; return; }

    /* Caps lock toggle (make only) */
    if (scancode == 0x3A) {
        kb.caps = !kb.caps;
        return;
    }

    /* Ignore all other break codes (key release) */
    if (scancode & 0x80)
        return;

    /* Bounds check */
    if (scancode >= 88)
        return;

    /* Look up ASCII */
    char c = kb.shift ? scancode_shifted[scancode] : scancode_unshifted[scancode];
    if (c == 0)
        return;

    /* Ctrl+letter → control character (e.g. Ctrl+C = 0x03, Ctrl+D = 0x04) */
    if (kb.ctrl && c >= 'a' && c <= 'z') {
        kb_buf_put((char)(c - 'a' + 1));
        return;
    }
    if (kb.ctrl && c >= 'A' && c <= 'Z') {
        kb_buf_put((char)(c - 'A' + 1));
        return;
    }

    /* Apply caps lock - flip case for letters */
    if (kb.caps) {
        if (c >= 'a' && c <= 'z')
            c -= 32;
        else if (c >= 'A' && c <= 'Z')
            c += 32;
    }

    kb_buf_put(c);
}

int keyboard_read(char *buf, unsigned int len)
{
    unsigned int count = 0;
    while (count < len && kb.tail != kb.head) {
        buf[count++] = kb.data[kb.tail];
        kb.tail = (kb.tail + 1) % KB_BUFFER_SIZE;
    }
    return (int)count;
}

static int kbd_owned = 0;

void kbd_set_owner(int owned)
{
    kbd_owned = owned;
}

int kbd_is_owned(void)
{
    return kbd_owned;
}
