#pragma once

#include <stdint.h>

/* in vbe.c */
uint32_t rgb(uint8_t r, uint8_t g, uint8_t b);

void init_fbdev(char *fbaddr, uint32_t width, uint32_t height, uint8_t depth,
        uint16_t pitch);
void writestr_fb(char *str);
void writechar_fb(char c);
