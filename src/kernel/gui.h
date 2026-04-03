#pragma once

#include <stdint.h>
#include <stdbool.h>

#define GUI_MAX_WINDOWS     16
#define GUI_TITLE_HEIGHT    20
#define GUI_BORDER          2
#define GUI_CLOSE_BTN_SIZE  14

/* GUI event types delivered to windows */
#define GUI_EVENT_NONE      0
#define GUI_EVENT_KEYPRESS  1
#define GUI_EVENT_MOUSE_BTN 2   /* button press/release inside client area */
#define GUI_EVENT_RESIZE    3   /* window was resized; x=new_w, y=new_h (client area) */

struct gui_event {
    uint8_t  type;
    /* GUI_EVENT_KEYPRESS */
    char     key;
    /* GUI_EVENT_MOUSE_BTN */
    uint16_t x, y;          /* position relative to client area */
    uint8_t  buttons;       /* MOUSE_BTN_* flags */
};

#define GUI_EVENT_QUEUE_SIZE 16

struct window {
    int         id;
    int32_t     x, y;          /* screen position of window (title bar top-left) */
    uint32_t    w, h;           /* total window width, total window height (incl title) */
    char        title[64];

    /* Per-window backbuffer for the client area only (w × (h - TITLE_HEIGHT) pixels) */
    uint32_t   *backbuffer;     /* allocated from kernel frames, NULL if not ready */
    uint32_t    bb_w, bb_h;     /* dimensions backbuffer was allocated at */

    bool        visible;
    bool        focused;
    bool        dirty;          /* needs recomposite */

    /* Input event queue */
    struct gui_event events[GUI_EVENT_QUEUE_SIZE];
    unsigned int ev_head, ev_tail;

    struct window *next;
};

/* Initialise the GUI subsystem and start the compositor kernel task */
void init_gui(void);

/* The compositor task entry point (run as a kernel task) */
void compositor_task(void);

/* Wake the compositor for cursor-only update */
void gui_wake(void);
/* Wake the compositor and force full scene redraw */
void gui_wake_scene(void);

/* Returns true if any windows are currently open */
bool gui_has_windows(void);

/*
 * Kernel-internal window API (also called from syscall handlers)
 */

/* Create a new window. Returns window ID, or -1 on failure. */
int  gui_window_create(int32_t x, int32_t y, uint32_t w, uint32_t h,
                       const char *title);

/* Destroy a window by ID */
void gui_window_destroy(int id);

/* Fill a rectangle in a window's client backbuffer */
void gui_window_fill_rect(int id, uint32_t x, uint32_t y,
                          uint32_t w, uint32_t h, uint32_t color);

/* Draw a 1-pixel outline rectangle in a window's client backbuffer */
void gui_window_draw_rect(int id, uint32_t x, uint32_t y,
                          uint32_t w, uint32_t h, uint32_t color);

/* Draw text into a window's client backbuffer at pixel (x,y) */
void gui_window_draw_text(int id, uint32_t x, uint32_t y,
                          const char *str, uint32_t fgcolor, uint32_t bgcolor);

/* Mark a window dirty and wake the compositor */
void gui_window_flip(int id);

/* Non-blocking poll for an event on a window. Returns 1 if event returned. */
int  gui_window_poll_event(int id, struct gui_event *ev);

/* Push an event into a window's queue (called from compositor/IRQ side) */
void gui_window_push_event(struct window *w, struct gui_event *ev);

/* Get current client area size of a window by ID. Returns 0 on success. */
int  gui_window_get_size(int id, uint32_t *cw, uint32_t *ch);
