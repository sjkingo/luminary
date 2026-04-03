/*
 * Userland stdio for Luminary OS.
 * vsnprintf core derived from Georges Menie's embedded printf
 * (LGPL), adapted for freestanding userspace.
 * printf/puts write to stdout via SYS_WRITE.
 */

#include "stdio.h"
#include "string.h"

/* ── SYS_WRITE stub (avoids depending on syscall.h) ─────────────────────── */

static void _write(const char *buf, unsigned int len)
{
    __asm__ volatile(
        "int $0x80"
        :
        : "a"(1), "b"(buf), "c"(len)
        : "memory"
    );
}

/* ── vsnprintf core ──────────────────────────────────────────────────────── */

#define PAD_RIGHT 1
#define PAD_ZERO  2

typedef struct {
    char        *buf;
    unsigned int pos;
    unsigned int size;  /* 0 = unbounded (sprintf) */
} _out_t;

static void _outc(_out_t *o, char c)
{
    if (o->buf) {
        if (o->size == 0 || o->pos < o->size - 1)
            o->buf[o->pos] = c;
        o->pos++;
    } else {
        _write(&c, 1);
    }
}

static int _puts_padded(_out_t *o, const char *s, int width, int pad)
{
    int pc = 0;
    int len = (int)strlen(s);
    int padchar = (pad & PAD_ZERO) ? '0' : ' ';

    if (width > len) {
        int npad = width - len;
        if (!(pad & PAD_RIGHT))
            while (npad--) { _outc(o, (char)padchar); pc++; }
        while (*s) { _outc(o, *s++); pc++; }
        if (pad & PAD_RIGHT)
            while (npad--) { _outc(o, ' '); pc++; }
    } else {
        while (*s) { _outc(o, *s++); pc++; }
    }
    return pc;
}

#define PRINT_BUF_LEN 12

static int _printi(_out_t *o, int i, int base, int sign,
                   int width, int pad, int letbase)
{
    char buf[PRINT_BUF_LEN];
    char *s = buf + PRINT_BUF_LEN - 1;
    int neg = 0;
    unsigned int u = (unsigned int)i;

    *s = '\0';
    if (i == 0) { *--s = '0'; return _puts_padded(o, s, width, pad); }
    if (sign && base == 10 && i < 0) { neg = 1; u = (unsigned int)-i; }
    while (u) {
        int t = (int)(u % (unsigned int)base);
        *--s = (char)(t >= 10 ? t + letbase - '0' - 10 + '0' : t + '0');
        u /= (unsigned int)base;
    }
    if (neg) {
        if (width && (pad & PAD_ZERO)) {
            _outc(o, '-');
            width--;
        } else {
            *--s = '-';
        }
    }
    return _puts_padded(o, s, width, pad);
}

static int _vsnprintf_core(_out_t *o, const char *fmt, va_list args)
{
    int pc = 0;
    char scr[2];

    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            _outc(o, *fmt); pc++;
            continue;
        }
        fmt++;
        if (*fmt == '\0') break;
        if (*fmt == '%') { _outc(o, '%'); pc++; continue; }

        int pad = 0, width = 0;
        if (*fmt == '-') { pad = PAD_RIGHT; fmt++; }
        while (*fmt == '0') { pad |= PAD_ZERO; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            if (width > 256) width = 256;
            fmt++;
        }
        if (*fmt == 'l') fmt++; /* consume length modifier */

        switch (*fmt) {
        case 's': {
            char *s = (char *)va_arg(args, int);
            pc += _puts_padded(o, s ? s : "(null)", width, pad);
            break;
        }
        case 'd':
            pc += _printi(o, va_arg(args, int), 10, 1, width, pad, 'a');
            break;
        case 'u':
            pc += _printi(o, va_arg(args, int), 10, 0, width, pad, 'a');
            break;
        case 'x':
            pc += _printi(o, va_arg(args, int), 16, 0, width, pad, 'a');
            break;
        case 'X':
            pc += _printi(o, va_arg(args, int), 16, 0, width, pad, 'A');
            break;
        case 'o':
            pc += _printi(o, va_arg(args, int), 8, 0, width, pad, 'a');
            break;
        case 'c':
            scr[0] = (char)va_arg(args, int);
            scr[1] = '\0';
            pc += _puts_padded(o, scr, width, pad);
            break;
        default:
            _outc(o, '%'); pc++;
            _outc(o, *fmt); pc++;
            break;
        }
    }

    if (o->buf && o->size > 0)
        o->buf[o->pos < o->size ? o->pos : o->size - 1] = '\0';

    return pc;
}

/* ── public API ──────────────────────────────────────────────────────────── */

int vsnprintf(char *buf, unsigned int size, const char *fmt, va_list args)
{
    _out_t o = { buf, 0, size };
    return _vsnprintf_core(&o, fmt, args);
}

int snprintf(char *buf, unsigned int size, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int r = vsnprintf(buf, size, fmt, args);
    va_end(args);
    return r;
}

int sprintf(char *buf, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    _out_t o = { buf, 0, 0 };
    int r = _vsnprintf_core(&o, fmt, args);
    if (buf) buf[o.pos] = '\0';
    va_end(args);
    return r;
}

int printf(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    _out_t o = { 0, 0, 0 };  /* buf=NULL → write directly to stdout */
    int r = _vsnprintf_core(&o, fmt, args);
    va_end(args);
    return r;
}

int puts(const char *s)
{
    unsigned int len = (unsigned int)strlen(s);
    _write(s, len);
    _write("\n", 1);
    return 0;
}

int putchar(int c)
{
    char ch = (char)c;
    _write(&ch, 1);
    return c;
}
