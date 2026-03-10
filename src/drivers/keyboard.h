#pragma once

#define KB_BUFFER_SIZE 256

void init_keyboard(void);
void keyboard_irq_handler(void);
int keyboard_read(char *buf, unsigned int len);
