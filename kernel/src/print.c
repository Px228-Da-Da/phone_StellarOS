#include "print.h"
#include "spinlock.h"
#include "uart.h"
#include "fb.h"
#include "usb.h"
#include "spinlock.h"
#include <stdarg.h>

/*
 * Замок печати.
 *
 * С появлением второго ядра вывод перестал быть безобидным: восемь ядер,
 * печатающих одновременно, дают на экране кашу из перемешанных посимвольно
 * строк, и первое же сообщение об ошибке становится нечитаемым — именно
 * тогда, когда оно нужнее всего.
 *
 * Замок берётся с запретом прерываний: kprintf вызывается и из обработчиков,
 * а прерывание на том же ядре внутри удерживаемого замка ждало бы само себя.
 */
static struct spinlock print_lock = SPINLOCK_INIT("print");

/* Единая точка вывода: всё, что печатаем, идёт в оба канала */
/*
 * Дублировать ли вывод на экран.
 *
 * Пока экран был единственным способом хоть что-то увидеть, отладка шла
 * прямо в кадр. Теперь есть консоль по USB, а кадр нужен под интерфейс:
 * рисовать поверх бегущего текста невозможно. По умолчанию оставляем
 * включённым — до появления консоли иначе не видно вообще ничего.
 */
static int fb_echo = 1;

void kprint_to_fb(int on)
{
    fb_echo = on;
}

static void emit(char c)
{
    uart_putc(c);
    if (fb_echo)
        fb_putc(c);
    usb_putc(c);        /* третий канал: терминал на компьютере */
}

static void emit_str(const char *s)
{
    while (*s)
        emit(*s++);
}

/*
 * Замок не отпускают дольше разумного.
 *
 * Печатаем один раз на замок и не чаще: если встали намертво, поток
 * сообщений не поможет, а вот забить кольцо консоли помешает разбору.
 * Рекурсию отсекаем отдельно — сам вывод тоже берёт замок, и попытка
 * пожаловаться на него же кончилась бы бесконечной жалобой.
 */
void spin_stuck(struct spinlock *l)
{
    static volatile u32 reporting;

    if (reporting)
        return;
    reporting = 1;

    kprintf("ЗАМОК    : %s НЕ ОТПУСКАЮТ, ДЕРЖИТ ЯДРО %lx, ЖДЁТ %lx\n",
            l->name ? l->name : "?", l->holder,
            read_mpidr() & 0x00FFFFFFUL);

    reporting = 0;
}

void kputs(const char *s)
{
    u64 flags = spin_lock_irq(&print_lock);

    emit_str(s);
    spin_unlock_irq(&print_lock, flags);
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
    u64 flags = spin_lock_irq(&print_lock);

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
            emit_str(s ? s : "(null)");
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
            emit_str("0x");
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
    spin_unlock_irq(&print_lock, flags);
}
