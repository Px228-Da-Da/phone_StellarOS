/*
 * Батарея через PMIC MT6358.
 *
 * Внутри PMIC есть свой аналого-цифровой преобразователь, и батарея
 * висит на его нулевом канале. Порядок работы простой: попросить
 * измерение, подождать, забрать результат вместе с признаком готовности.
 *
 * Числа не подобраны, а взяты из вендорных исходников merlin:
 *   адреса регистров  include/linux/mfd/mt6358/registers.h
 *   делитель 3:1 и 128 усреднений   mt6358.dtsi, узел batadc
 *   опорное напряжение 1800 мВ      drivers/iio/adc/mt635x-auxadc.c
 *   кривая разряда    bat_setting/mt6768_battery_table.dtsi
 */
#include "battery.h"
#include "pmic.h"
#include "io.h"

#if defined(BOARD_MERLIN)

/* --- Регистры PMIC ------------------------------------------------- */
#define MT6358_TOPSTATUS        0x0028      /* бит 2: питание подключено */
#define TOPSTATUS_CHRDET        (1U << 2)

#define MT6358_AUXADC_ADC0      0x1088      /* результат нулевого канала */
#define MT6358_AUXADC_RQST0     0x1108      /* запрос измерения          */
#define AUXADC_RQST_BATADC      (1U << 0)

#define AUXADC_RDY              (1U << 15)  /* результат готов           */
#define AUXADC_BITS             15          /* разрядность канала        */
#define AUXADC_VREF_MV          1800        /* опорное напряжение        */
#define BATADC_DIV              3           /* делитель на входе, 3:1    */

/*
 * Кривая разряда этой батареи.
 *
 * Собрана из вендорного профиля battery0_profile_t2 — сотня точек
 * «остаток заряда — напряжение» для комнатной температуры. Здесь оставлены
 * опорные, между ними считаем линейно; ближе к нулю точки чаще, потому
 * что там напряжение падает круто и редкая сетка врала бы сильнее всего.
 *
 * Важная оговорка: это напряжение БЕЗ нагрузки. Под нагрузкой оно
 * проседает, и процент выходит заниженным — настоящий счётчик заряда
 * учитывает ещё и ток, и внутреннее сопротивление. Нам этого пока
 * достаточно: вопрос «сколько осталось и заряжается ли» такой точности
 * не требует.
 */
struct ocv_point {
    u16 mv;
    u8  percent;
};

static const struct ocv_point curve[] = {
    { 4418, 100 },
    { 4304,  90 },
    { 4182,  80 },
    { 4069,  70 },
    { 3969,  60 },
    { 3867,  50 },
    { 3816,  40 },
    { 3783,  30 },
    { 3745,  20 },
    { 3712,  15 },
    { 3690,  10 },
    { 3667,   7 },
    { 3580,   5 },
    { 3397,   3 },
    { 3192,   2 },
    { 2917,   0 },
};

static u32 mv_to_percent(u32 mv)
{
    const u32 n = sizeof(curve) / sizeof(curve[0]);

    if (mv >= curve[0].mv)
        return 100;
    for (u32 i = 1; i < n; i++) {
        if (mv >= curve[i].mv) {
            /* Линейно между двумя соседними точками кривой */
            u32 hi_mv = curve[i - 1].mv, lo_mv = curve[i].mv;
            u32 hi_p  = curve[i - 1].percent, lo_p = curve[i].percent;

            return lo_p + ((mv - lo_mv) * (hi_p - lo_p)) / (hi_mv - lo_mv);
        }
    }
    return 0;
}

/* Пауза по системному счётчику: таймера ядра здесь может ещё не быть */
static void udelay(u32 us)
{
    u64 end = read_cntpct() + (read_cntfrq() / 1000000) * us + 1;

    while (read_cntpct() < end)
        ;
}

void battery_read(struct battery_state *st)
{
    u16 top = 0, raw = 0;

    st->valid = 0;
    st->raw = 0;
    st->mv = 0;
    st->percent = 0;
    st->charging = 0;

    if (pmic_read(MT6358_TOPSTATUS, &top) == 0)
        st->charging = (top & TOPSTATUS_CHRDET) ? 1 : 0;

    /* Просим измерение нулевого канала */
    if (pmic_write(MT6358_AUXADC_RQST0, AUXADC_RQST_BATADC) != 0)
        return;

    /* Столько преобразователь усредняет: 128 замеров по 10 мкс */
    udelay(1280);

    /* Ждём признак готовности. Ограничение обязательно: если канал
     * настроен не так, ядро не должно повиснуть здесь навсегда. */
    for (u32 try = 0; try < 50; try++) {
        if (pmic_read(MT6358_AUXADC_ADC0, &raw) != 0)
            return;
        if (raw & AUXADC_RDY)
            break;
        udelay(200);
    }
    if (!(raw & AUXADC_RDY))
        return;

    raw &= (1U << AUXADC_BITS) - 1;
    st->raw = raw;

    /* Напряжение на входе делителя: код * опорное / полная шкала,
     * умноженное на коэффициент делителя. */
    st->mv = ((u32)raw * BATADC_DIV * AUXADC_VREF_MV) >> AUXADC_BITS;
    st->percent = mv_to_percent(st->mv);
    st->valid = 1;
}

#else   /* в эмуляторе батареи нет */

void battery_read(struct battery_state *st)
{
    st->mv = 0;
    st->percent = 0;
    st->charging = 0;
    st->valid = 0;
    st->raw = 0;
}

#endif
