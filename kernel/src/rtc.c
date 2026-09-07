/*
 * Часы реального времени. Живут в контроллере питания.
 *
 * Отдельная микросхема, у которой своё питание от батарейки: она идёт,
 * пока телефон выключен, и знает настоящее время суток. Процессорный
 * таймер этого не умеет вовсе — он считает от включения, и «сколько
 * система работает» к часам на экране отношения не имеет.
 *
 * Адреса взяты из вендорских источников, а не подобраны:
 *
 *   MT6358_RTC_BASE  0x0588   — drivers/mfd/mt6397-core.c
 *   RTC_TC_SEC       0x000a   — include/linux/mfd/mt6397/rtc.h
 *
 * Дальше в том же заголовке сказано, что минуты, часы, день и прочее
 * идут подряд ЗА секундами, и перечислены их порядковые номера:
 *
 *   RTC_OFFSET_SEC 0, MIN 1, HOUR 2, DOM 3, DOW 4, MTH 5, YEAR 6
 *
 * Шаг между регистрами — два байта. Это не догадка: там же объявлено
 * RTC_AL_SEC как 0x0018, а по нашей раскладке это ровно 0x0A + 7*2, то
 * есть следующий регистр сразу за семью временными. Сходится.
 */
#include "rtc.h"
#include "pmic.h"

#if defined(BOARD_MERLIN)

#define RTC_BASE        0x0588
#define RTC_TC_SEC      (RTC_BASE + 0x000A)
#define RTC_STEP        2               /* регистры идут через два байта */

#define TC_SEC          0
#define TC_MIN          1
#define TC_HOUR         2
#define TC_DOM          3
#define TC_DOW          4
#define TC_MTH          5
#define TC_YEAR         6

static int tc(u32 idx, u16 *out)
{
    return pmic_read(RTC_TC_SEC + idx * RTC_STEP, out);
}

int rtc_now(struct rtc_time *t)
{
    u16 v;

    if (!t)
        return -1;

    /*
     * Читаем секунды до и после остального.
     *
     * Часы идут независимо от нас, и чтение семи регистров подряд может
     * застать смену минуты: секунды успеют обнулиться, а минуты — ещё
     * нет, и на экране выскочит время на минуту назад. Поэтому если
     * секунды за время чтения перевалили через ноль, читаем всё заново.
     * Такое случается раз в минуту и обходится одним лишним разом.
     */
    for (int again = 0; again < 2; again++) {
        u16 sec0;

        if (tc(TC_SEC, &sec0) != 0)
            return -1;

        if (tc(TC_MIN, &v) != 0)
            return -1;
        t->min = (u8)(v & 0x3F);
        if (tc(TC_HOUR, &v) != 0)
            return -1;
        t->hour = (u8)(v & 0x1F);
        if (tc(TC_DOM, &v) != 0)
            return -1;
        t->day = (u8)(v & 0x1F);
        if (tc(TC_DOW, &v) != 0)
            return -1;
        t->dow = (u8)(v & 0x07);
        if (tc(TC_MTH, &v) != 0)
            return -1;
        t->month = (u8)(v & 0x0F);
        if (tc(TC_YEAR, &v) != 0)
            return -1;
        t->year = (u8)(v & 0x7F);       /* лет от 2000-го */

        if (tc(TC_SEC, &v) != 0)
            return -1;
        t->sec = (u8)(v & 0x3F);

        if (t->sec >= (sec0 & 0x3F))
            return 0;                   /* через ноль не перескочили */
    }
    return 0;
}

#else

/* На эмуляторе контроллера питания нет; часов тоже нет. */
int rtc_now(struct rtc_time *t)
{
    (void)t;
    return -1;
}

#endif
