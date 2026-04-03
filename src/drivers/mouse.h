#pragma once

#include <stdint.h>

/* A decoded mouse event from the ring buffer */
struct mouse_event {
    int16_t dx;         /* relative X movement (positive = right) */
    int16_t dy;         /* relative Y movement (positive = down) */
    uint8_t buttons;    /* bit 0 = left, bit 1 = right, bit 2 = middle */
};

#define MOUSE_BTN_LEFT   (1 << 0)
#define MOUSE_BTN_RIGHT  (1 << 1)
#define MOUSE_BTN_MIDDLE (1 << 2)

/* Absolute cursor position, clamped to screen bounds */
extern uint32_t mouse_x;
extern uint32_t mouse_y;

/* Last known button state (MOUSE_BTN_* flags) */
extern uint8_t mouse_buttons;

void init_mouse(void);
void mouse_irq_handler(void);

/* Non-blocking read. Returns 1 if an event was available, 0 if not. */
int mouse_read(struct mouse_event *ev);
