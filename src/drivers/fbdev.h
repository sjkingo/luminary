#pragma once

#include <stdbool.h>
#include <stdint.h>

/* in vbe.c */
uint32_t rgb(uint8_t r, uint8_t g, uint8_t b);

/* Returns true if the framebuffer console has been initialized */
bool fbdev_is_ready(void);

void init_fbdev(char *fbaddr, uint32_t width, uint32_t height, uint8_t depth,
        uint16_t pitch);

/* Write a single character to the framebuffer console.
 * This is the kernel-only dumb console used by printk and panic.
 * Full console rendering (scrollback, line editing) is handled by
 * userland fbcon via /dev/console and /dev/fb0. */
void writechar_fb(char c);
