#ifndef ULIB_H
#define ULIB_H

/*
 * Всё, что есть у программы в EL0.
 *
 * Библиотеки нет и не будет в обычном смысле: единственный способ
 * попросить что-нибудь у мира — команда svc. Здесь она обёрнута в
 * функции, чтобы программы писались на Си, а не на ассемблере.
 *
 * Номера вызовов берём из общего с ядром заголовка: договор один, и
 * дублировать его здесь значило бы завести второй, который однажды
 * разойдётся с первым.
 */
#include "syscall.h"

typedef unsigned char       u8;
typedef unsigned short      u16;
typedef unsigned int        u32;
typedef unsigned long       u64;
typedef long                s64;

/*
 * Сама команда. Номер в x8, аргументы в x0..x5, ответ в x0 — ровно то,
 * что ждёт обработчик в ядре.
 *
 * "memory" в списке испорченного обязателен: ядро может записать в нашу
 * память (например, событие касания), и компилятор должен знать, что
 * после вызова его представление о памяти устарело.
 */
static inline s64 sys(u64 nr, u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5)
{
    register u64 x8 __asm__("x8") = nr;
    register u64 x0 __asm__("x0") = a0;
    register u64 x1 __asm__("x1") = a1;
    register u64 x2 __asm__("x2") = a2;
    register u64 x3 __asm__("x3") = a3;
    register u64 x4 __asm__("x4") = a4;
    register u64 x5 __asm__("x5") = a5;

    __asm__ volatile("svc #0"
                     : "+r"(x0)
                     : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5)
                     : "memory", "cc");
    return (s64)x0;
}

static inline void exit(u64 code)
{
    sys(SYS_EXIT, code, 0, 0, 0, 0, 0);
    for (;;)
        ;                       /* сюда управление уже не вернётся */
}

static inline u64 ustrlen(const char *s)
{
    u64 n = 0;

    while (s[n])
        n++;
    return n;
}

static inline s64 write(const char *s)
{
    return sys(SYS_WRITE, (u64)s, ustrlen(s), 0, 0, 0, 0);
}

static inline void yield(void)      { sys(SYS_YIELD, 0, 0, 0, 0, 0, 0); }
static inline void sleep_ms(u64 ms) { sys(SYS_SLEEP_MS, ms, 0, 0, 0, 0, 0); }
static inline u64  uptime_ms(void)  { return (u64)sys(SYS_UPTIME_MS, 0,0,0,0,0,0); }
static inline u64  getpid(void)     { return (u64)sys(SYS_GETPID, 0,0,0,0,0,0); }

static inline s64 spawn(u32 image)  { return sys(SYS_SPAWN, image, 0,0,0,0,0); }
static inline s64 wait_task(u64 id) { return sys(SYS_WAIT, id, 0,0,0,0,0); }

/* Окно: буфер в нашей памяти, показывает его контроллер дисплея */
static inline u32 *window(u32 w, u32 h)
{
    return (u32 *)(u64)sys(SYS_WINDOW, w, h, 0, 0, 0, 0);
}

static inline s64 present(u32 x, u32 y)
{
    return sys(SYS_PRESENT, x, y, 0, 0, 0, 0);
}

/*
 * Показать половину буфера окна: 0 — первую, 1 — вторую.
 *
 * Двойная буферизация: рисуем в ту, что сейчас не показывается, и
 * просим показать её целиком. На экране не бывает наполовину
 * нарисованного — переключение происходит между кадрами.
 */
static inline s64 flip(u32 half)
{
    return sys(SYS_FLIP, half, 0, 0, 0, 0, 0);
}

/*
 * Перезагрузить телефон. Не возвращается.
 *
 * Спросить об этом может любая программа: разделения на своих и чужих у
 * системы пока нет, и притворяться, что оно есть, было бы хуже, чем
 * честно его не иметь.
 */
static inline void reboot(void)
{
    sys(SYS_REBOOT, 0, 0, 0, 0, 0, 0);
    for (;;)
        ;
}

static inline s64 window_close(void)
{
    return sys(SYS_CLOSE, 0, 0, 0, 0, 0, 0);
}

/*
 * Написать в своём окне шрифтом ядра.
 *
 * bg с нулевой прозрачностью — писать только буквы, не трогая то, что
 * под ними. Непрозрачный bg закрашивает строку целиком за один проход:
 * так меняющийся текст не мигает, потому что пустого промежуточного
 * состояния не возникает.
 */
static inline s64 text(u32 x, u32 y, u32 scale, u32 fg, u32 bg,
                       const char *s)
{
    return sys(SYS_TEXT, x, y, scale, fg | ((u64)bg << 32),
               (u64)s, ustrlen(s));
}

/*
 * Строка постоянной ширины: дополняем пробелами до width знаков.
 *
 * Нужна там же, где непрозрачный фон: если новая надпись короче старой,
 * хвост старой останется на экране. Пробелы с непрозрачным фоном его и
 * затирают — тем же единственным проходом.
 */
static inline void upad(char *s, u32 width)
{
    u32 n = 0;

    while (s[n])
        n++;
    while (n < width)
        s[n++] = ' ';
    s[n] = 0;
}

/* Размеры экрана: ширина в старшей половине, высота в младшей */
static inline void screen_size(u32 *w, u32 *h)
{
    u64 v = (u64)sys(SYS_SCREEN, 0, 0, 0, 0, 0, 0);

    if (w)
        *w = (u32)(v >> 32);
    if (h)
        *h = (u32)v;
}

/*
 * Касание. Раскладка обязана совпадать с той, что ядро копирует нам в
 * память, — см. TOUCH_EVENT_BYTES в общем заголовке.
 */
struct touch {
    u16 x, y;
    u8  id;
    u8  action;                 /* 0 нажали, 1 ведут, 2 отпустили */
};

#define TOUCH_DOWN  0
#define TOUCH_MOVE  1
#define TOUCH_UP    2

/* Ждать касания. Возвращает 1, когда событие положено в t. */
static inline s64 input(struct touch *t)
{
    return sys(SYS_INPUT, (u64)t, 0, 0, 0, 0, 0);
}

/* --- Мелочи, которых больше взять негде ------------------------- */

static inline void ufill(u32 *buf, u32 count, u32 color)
{
    for (u32 i = 0; i < count; i++)
        buf[i] = color;
}

/* Прямоугольник в окне шириной pitch пикселей */
static inline void urect(u32 *buf, u32 pitch, u32 x, u32 y,
                         u32 w, u32 h, u32 color)
{
    for (u32 row = 0; row < h; row++) {
        u32 *line = buf + (u64)(y + row) * pitch + x;

        for (u32 col = 0; col < w; col++)
            line[col] = color;
    }
}

/* Рамка толщиной t */
static inline void uframe(u32 *buf, u32 pitch, u32 x, u32 y,
                          u32 w, u32 h, u32 t, u32 color)
{
    urect(buf, pitch, x, y, w, t, color);
    urect(buf, pitch, x, y + h - t, w, t, color);
    urect(buf, pitch, x, y, t, h, color);
    urect(buf, pitch, x + w - t, y, t, h, color);
}

/*
 * Число десятичными цифрами в буфер. Возвращает длину.
 *
 * Своё, потому что взять неоткуда: библиотеки у нас нет, а печатать
 * числа надо всем.
 */
static inline u32 unum(char *dst, u64 v)
{
    char tmp[24];
    u32 n = 0, i = 0;

    do {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);

    while (n)
        dst[i++] = tmp[--n];
    dst[i] = 0;
    return i;
}

/* Склеить строку и число: "КАСАНИЙ: 42" */
static inline void ulabel(char *dst, const char *label, u64 v)
{
    u32 i = 0;

    while (*label)
        dst[i++] = *label++;
    unum(dst + i, v);
}

#endif
