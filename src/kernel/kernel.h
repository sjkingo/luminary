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

/* Debug logging macro. Usage: DBGK("loading segment %d\n", i)
 * Compiles to nothing unless -DDEBUG is set.
 * Output goes to serial only — does not clutter the framebuffer console.
 * Format: basename.c:line(func): message */
#define _DBGK_BASENAME(path) \
    (__builtin_strrchr(path, '/') ? __builtin_strrchr(path, '/') + 1 : (path))

static inline const char *path_basename(const char *path) {
    const char *s = path;
    for (const char *p = path; *p; p++)
        if (*p == '/') s = p + 1;
    return s;
}
#ifdef DEBUG
#define DBGK(fmt, ...) printk_serial("%s:%d(%s): " fmt, \
    _DBGK_BASENAME(__FILE__), __LINE__, __func__, ##__VA_ARGS__)
#else
#define DBGK(fmt, ...) ((void)0)
#endif

#define panic(msg) (real_panic(msg, __FILE__, __LINE__, __func__))

/* Kernel panic: print given message and halt the CPU */
void real_panic(char *msg, char const *file, int line, char const *func)
    __attribute__((noreturn));

/* in cpu.c */
void init_cpu(void);
void cpu_reset_fault_counter(void);
