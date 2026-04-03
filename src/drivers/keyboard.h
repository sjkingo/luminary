#pragma once

#define KB_BUFFER_SIZE 256

/* Sentinel bytes for special keys (non-printable, placed in ring buffer) */
#define KEY_PGUP  0x01
#define KEY_PGDN  0x02

void init_keyboard(void);
void keyboard_irq_handler(void);
int keyboard_read(char *buf, unsigned int len);
