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

/* Дополнить строку пробелами до постоянной ширины */
static void pad(char *s, u32 width)
{
    u32 n = 0;

    while (s[n])
        n++;
    while (n < width)
        s[n++] = ' ';
    s[n] = 0;
}

/*
 * Полосы уже нарисованы: дальше меняется только текст.
 *
 * Это и есть лечение мигания. Раньше каждое обновление начиналось с
 * закраски полосы целиком, а потом по ней писался текст — и между этими
 * двумя действиями контроллер дисплея успевал показать пустую полосу.
 * Раз в секунду, на самом виду. Ровно та же болезнь, что когда-то была в
 * оболочке, и лечится она так же: каждый пиксель меняется ровно один
 * раз, а не дважды.
 *
 * Поэтому фон под полосами кладётся однажды, а часы и заряд пишутся с
 * непрозрачным фоном и дополняются пробелами до постоянной ширины: новая
 * надпись закрывает старую тем же проходом, которым рисует себя.
 */
static int bars_painted;

void ui_status_repaint(void)
{
    bars_painted = 0;
    ui_status_draw();
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

    if (!bars_painted) {
        u32 bar_w = sw / 3;
        u32 bar_x = (sw - bar_w) / 2;
        u32 bar_y = sh - UI_HOME_H / 2 - 3;

        fb_fill_rect(0, 0, sw, UI_STATUS_H, COL_BAR);
        fb_fill_rect(0, sh - UI_HOME_H, sw, UI_HOME_H, COL_BAR);
        fb_fill_rect(bar_x, bar_y, bar_w, 6, COL_HOME);
        bars_painted = 1;
    }

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
    pad(line, 8);
    fb_text(220, 26, 2, COL_TEXT, COL_BAR, line);

    /* --- заряд --- */
    battery_last(&bat);
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

    /*
     * Пишем от постоянного места, а не от правого края.
     *
     * Правое выравнивание пришлось бы двигать вслед за длиной надписи, и
     * хвост прежней оставался бы на экране — либо его пришлось бы
     * стирать отдельным проходом, то есть мигать.
     */
    pad(line, 16);
    fb_text(sw > 480 ? sw - 480 : 0, 26, 2,
            bat.valid && bat.percent < 15 ? 0xFFFF6B6B : COL_TEXT,
            COL_BAR, line);

    (void)COL_DIM;
}

static void ui_status_task(void *arg)
{
    u32 n = 0;

    (void)arg;

    for (;;) {
        ui_status_draw();

        /*
         * Первые секунды перерисовываем чаще.
         *
         * В кадр при загрузке пишет не только эта задача, и если полосы
         * кто-то затрёт, ждать целую секунду до починки — значит показать
         * человеку пустой верх экрана ровно в тот момент, когда он на
         * него смотрит. Дальше в кадре никто не хозяйничает, и раз в
         * секунду хватает: там только часы и заряд.
         */
        task_sleep_ms(n < 20 ? 250 : 1000);
        n++;
    }
}

void ui_status_start(void)
{
    task_create("полосы", ui_status_task, NULL);
}
