#pragma once

#define SERIAL_COM1_PORT 0x3F8

/* Initialise the serial driver with output to COM1 */
void serial_init(void);

/* Write a single character to COM1 (busy-waits for transmit ready) */
void write_serial(char a);
