#include "print.h"
#include "uart.h"
#include "fb.h"
#include <stdarg.h>

/* Единая точка вывода: всё, что печатаем, идёт в оба канала */
static void emit(char c)
{
    uart_putc(c);
    fb_putc(c);
}

void kputs(const char *s)
{
    while (*s)
        emit(*s++);
}

static void emit_u64(u64 v, unsigned base, int width, char pad)
{
    static const char digits[] = "0123456789abcdef";
    char buf[24];
    int i = 0;

    if (v == 0)
        buf[i++] = '0';
    while (v) {
        buf[i++] = digits[v % base];
        v /= base;
    }
    while (i < width)
        buf[i++] = pad;
    while (i--)
        emit(buf[i]);
}

void kprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);

    for (; *fmt; fmt++) {
        int is_long = 0, width = 0;
        char pad = ' ';

        if (*fmt != '%') {
            emit(*fmt);
            continue;
        }
        fmt++;

        if (*fmt == '0') { pad = '0'; fmt++; }
        while (*fmt >= '0' && *fmt <= '9')
            width = width * 10 + (*fmt++ - '0');
        if (*fmt == 'l') { is_long = 1; fmt++; }

        switch (*fmt) {
        case 's': {
            const char *s = va_arg(ap, const char *);
            kputs(s ? s : "(null)");
            break;
        }
        case 'c':
            emit((char)va_arg(ap, int));
            break;
        case 'd': {
            s64 v = is_long ? va_arg(ap, s64) : va_arg(ap, s32);
            if (v < 0) { emit('-'); v = -v; }
            emit_u64((u64)v, 10, width, pad);
            break;
        }
        case 'u': {
            u64 v = is_long ? va_arg(ap, u64) : va_arg(ap, u32);
            emit_u64(v, 10, width, pad);
            break;
        }
        case 'x': {
            u64 v = is_long ? va_arg(ap, u64) : va_arg(ap, u32);
            emit_u64(v, 16, width, pad);
            break;
        }
        case 'p':
            kputs("0x");
            emit_u64((u64)va_arg(ap, void *), 16, 16, '0');
            break;
        case '%':
            emit('%');
            break;
        default:
            emit('%');
            emit(*fmt);
        }
    }
    va_end(ap);
}
