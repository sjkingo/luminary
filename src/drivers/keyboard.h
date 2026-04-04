#pragma once

#define KB_BUFFER_SIZE 256

/* Sentinel bytes for special keys (non-printable, placed in ring buffer) */
#define KEY_PGUP  0x01
#define KEY_PGDN  0x02

void init_keyboard(void);
void keyboard_irq_handler(void);
int keyboard_read(char *buf, unsigned int len);

/* Keyboard ownership: when owned (1), stdin_read_op yields without consuming
 * input so all keystrokes go to the GUI compositor. Clear (0) to return
 * keyboard to the shell. */
void kbd_set_owner(int owned);
int  kbd_is_owned(void);
