#ifndef FB_H
#define FB_H
#include "types.h"

/* Цвета в формате 0x00RRGGBB (реальный порядок каналов панели уточняется опытом) */
#define COLOR_BLACK   0x00000000
#define COLOR_WHITE   0x00FFFFFF
#define COLOR_RED     0x00FF0000
#define COLOR_GREEN   0x0000FF00
#define COLOR_BLUE    0x000000FF
#define COLOR_YELLOW  0x00FFFF00
#define COLOR_CYAN    0x0000FFFF

int  fb_init(void);                     /* 0 = ок, экран есть */
int  fb_available(void);
void fb_clear(u32 color);
void fb_fill_rect(u32 x, u32 y, u32 w, u32 h, u32 color);
void fb_putc(char c);                   /* текстовая консоль поверх fb */
void fb_set_colors(u32 fg, u32 bg);
void fb_info(u64 *base, u32 *w, u32 *h, u32 *stride);

#endif
