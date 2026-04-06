#pragma once

#define KB_BUFFER_SIZE 256

/* Sentinel bytes for special keys (non-printable, placed in ring buffer) */
#define KEY_PGUP  0x01
#define KEY_PGDN  0x02
#define KEY_UP    0x10
#define KEY_DOWN  0x11
#define KEY_LEFT  0x12
#define KEY_RIGHT 0x13
#define KEY_HOME  0x14
#define KEY_END   0x15
#define KEY_DEL   0x16
#define KEY_ALT_F4 0x17   /* Alt+F4 — close focused window */

void init_keyboard(void);
void keyboard_irq_handler(void);
int keyboard_read(char *buf, unsigned int len);

/* Keyboard ownership: when owned (1), stdin_read_op yields without consuming
 * input so all keystrokes go to the GUI compositor. Clear (0) to return
 * keyboard to the shell. */
void kbd_set_owner(int owned);
int  kbd_is_owned(void);
