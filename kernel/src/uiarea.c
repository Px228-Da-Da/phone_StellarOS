/*
 * Системные полосы: состояние сверху, «домой» снизу.
 *
 * Рисуются прямо в кадр — в тот самый нулевой слой, который лежит под
 * окнами приложений. Приложению они не мешают и не могут помешать: его
 * окно физически меньше экрана и стоит внутри рабочей области.
 *
 * Почему это делает ядро, а не оболочка. Полоса состояния должна быть
 * видна ВСЕГДА — и когда оболочка занята, и когда её сняли за ошибку, и
 * когда поверх лежит чужое приложение во весь экран. Отдать её программе
 * значит согласиться, что однажды её не будет.
 *
 * Обновляется раз в секунду отдельной задачей: там часы и заряд, и
 * меняются они не чаще. Две узкие полосы стоят копейки — вместе это
 * полторы сотни строк экрана против двух с лишним тысяч.
 */
#include "uiarea.h"
#include "fb.h"
#include "font.h"
#include "sched.h"
#include "timer.h"
#include "battery.h"
#include "print.h"
#include "string.h"

#define COL_BAR     0xFF05070E      /* полосы темнее рабочей области */
#define COL_TEXT    0xFF9FB4D8
#define COL_DIM     0xFF4A5A7A
#define COL_HOME    0xFF7F8DA8

void ui_area(u32 *x, u32 *y, u32 *w, u32 *h)
{
    u64 base;
    u32 sw, sh, stride;

    fb_info(&base, &sw, &sh, &stride);

    if (x)
        *x = 0;
    if (y)
        *y = UI_STATUS_H;
    if (w)
        *w = sw;
    if (h)
        *h = (sh > UI_STATUS_H + UI_HOME_H) ? sh - UI_STATUS_H - UI_HOME_H : sh;
}

/* Число в строку: своё, потому что взять негде */
static u32 num(char *dst, u64 v)
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

void ui_status_draw(void)
{
    u64 base;
    u32 sw, sh, stride;
    char line[64];
    u32 n;
    u64 sec;
    struct battery_state bat;

    if (!fb_available())
        return;

    fb_info(&base, &sw, &sh, &stride);
    if (sh <= UI_STATUS_H + UI_HOME_H)
        return;

    /* --- полоса состояния --- */
    fb_fill_rect(0, 0, sw, UI_STATUS_H, COL_BAR);

    /*
     * Часы слева, но не под вырезом камеры: у merlin он в левом верхнем
     * углу, и надпись под ним просто не видна. Отступаем от края
     * заведомо больше, чем занимает вырез.
     */
    sec = timer_uptime_ms() / 1000;
    n = 0;
    n += num(line + n, sec / 60);
    line[n++] = ':';
    if (sec % 60 < 10)
        line[n++] = '0';
    n += num(line + n, sec % 60);
    line[n] = 0;
    fb_text(220, 26, 2, COL_TEXT, COL_BAR, line);

    /* --- заряд справа --- */
    battery_read(&bat);
    n = 0;
    if (bat.valid) {
        n += num(line + n, bat.percent);
        line[n++] = '%';

        /*
         * Рядом с процентом — ток. Он честнее всего показывает, что
         * происходит: минус значит разряжается, плюс — заряжается, и
         * никакого «кабель воткнут, а телефон садится».
         */
        if (battery_current_valid() && bat.current_ma) {
            int ma = bat.current_ma;

            line[n++] = ' ';
            line[n++] = (ma < 0) ? '-' : '+';
            n += num(line + n, (u32)(ma < 0 ? -ma : ma));
            line[n++] = ' ';
            line[n++] = 'm';
            line[n++] = 'A';
        }
        line[n] = 0;
    } else {
        const char *q = "БАТАРЕЯ?";

        while (*q)
            line[n++] = *q++;
        line[n] = 0;
    }
    {
        u32 w = fb_text_width(2, line);

        fb_text(sw > w + 32 ? sw - w - 32 : 0, 26, 2,
                bat.valid && bat.percent < 15 ? 0xFFFF6B6B : COL_TEXT,
                COL_BAR, line);
    }

    /* --- полоска «домой» --- */
    {
        u32 bar_w = sw / 3;
        u32 bar_x = (sw - bar_w) / 2;
        u32 bar_y = sh - UI_HOME_H / 2 - 3;

        fb_fill_rect(0, sh - UI_HOME_H, sw, UI_HOME_H, COL_BAR);
        fb_fill_rect(bar_x, bar_y, bar_w, 6, COL_HOME);
    }

    (void)COL_DIM;
}

static void ui_status_task(void *arg)
{
    (void)arg;

    for (;;) {
        ui_status_draw();
        task_sleep_ms(1000);
    }
}

void ui_status_start(void)
{
    task_create("полосы", ui_status_task, NULL);
}
