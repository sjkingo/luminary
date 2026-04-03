#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "kernel/printk.h"

extern struct multiboot_info *mb_info;

struct kernel_time {
    unsigned long uptime_ms;
};
extern struct kernel_time timekeeper;

extern bool startup_complete;

/* Debug logging macro. Usage: DBGK("elf", "loading segment %d\n", i)
 * Compiles to nothing unless -DDEBUG is set.
 * Output goes to serial only — does not clutter the framebuffer console. */
#ifdef DEBUG
#define DBGK(subsys, fmt, ...) printk_serial("DEBUG(" subsys "): " fmt, ##__VA_ARGS__)
#else
#define DBGK(subsys, fmt, ...) ((void)0)
#endif

#define panic(msg) (real_panic(msg, __FILE__, __LINE__, __func__))

/* Kernel panic: print given message and halt the CPU */
void real_panic(char *msg, char const *file, int line, char const *func)
    __attribute__((noreturn));

/* in cpu.c */
void init_cpu(void);
void cpu_reset_fault_counter(void);
