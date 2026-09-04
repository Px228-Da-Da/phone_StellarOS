#ifndef FB_H
#define FB_H
#include "types.h"

/*
 * Цвета в формате 0xAARRGGBB.
 *
 * Альфа обязана быть 0xFF, а не нулём. Панель merlin работает в ARGB8888,
 * и нулевая альфа означает «полностью прозрачно»: нарисованное не видно,
 * сквозь него просвечивает чёрный фон. Именно так выглядела первая рабочая
 * прошивка — экран чернел (наша заливка доходила до буфера), но текст на
 * нём не появлялся, потому что был прозрачным.
 */
#define COLOR_BLACK   0xFF000000
#define COLOR_WHITE   0xFFFFFFFF
#define COLOR_RED     0xFFFF0000
#define COLOR_GREEN   0xFF00FF00
#define COLOR_BLUE    0xFF0000FF
#define COLOR_YELLOW  0xFFFFFF00
#define COLOR_CYAN    0xFF00FFFF

int  fb_init(void);                     /* 0 = ок, экран есть */
int  fb_available(void);
void fb_clear(u32 color);
void fb_fill_rect(u32 x, u32 y, u32 w, u32 h, u32 color);
void fb_putc(char c);                   /* текстовая консоль поверх fb */
void fb_set_colors(u32 fg, u32 bg);
void fb_info(u64 *base, u32 *w, u32 *h, u32 *stride);

#endif
