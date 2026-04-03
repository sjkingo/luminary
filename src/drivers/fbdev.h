#pragma once

#include <stdbool.h>
#include <stdint.h>

#define FBDEV_MAX_COLS 256
#define FBDEV_MAX_ROWS 128
#define FBDEV_HISTORY_ROWS 300   /* total rows kept in scrollback (>= MAX_ROWS) */
#define FBDEV_SCROLL_STEP 5      /* rows to scroll per Page Up/Down keypress */

/* in vbe.c */
uint32_t rgb(uint8_t r, uint8_t g, uint8_t b);

/* Returns true if the framebuffer console has been initialized */
bool fbdev_is_ready(void);

void init_fbdev(char *fbaddr, uint32_t width, uint32_t height, uint8_t depth,
        uint16_t pitch);

/* Write a single character to the framebuffer console */
void writechar_fb(char c);

/* Write a string to the framebuffer console */
void writestr_fb(char *str);

/* Write a string to a specific row (used for status line).
 * Clears the row first, does not affect cursor position.
 */
void writeline_fb(uint32_t row, char *str, uint32_t fgcolor, uint32_t bgcolor);

/* Scroll the console view up/down through scrollback history */
void fbdev_scroll_up(void);
void fbdev_scroll_down(void);
