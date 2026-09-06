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

/*
 * Счётчик заряда FGADC — тот самый «отдельный блок с измерительным
 * резистором».
 *
 * Адреса и порядок работы взяты из вендорных исходников merlin, а не
 * подобраны:
 *   регистры        include/linux/mfd/mt6358/registers.h
 *   поля            mt-plat/mt6768/include/mach/upmu_hw.h
 *                   PMIC_FG_CURRENT_OUT  = FGADC_CUR_CON0, 16 бит
 *                   PMIC_FG_LATCHDATA_ST = FGADC_CON1, бит 15
 *   порядок чтения  pmic/mt6358/v1/mt6358_gauge.c, fgauge_read_current
 *   единица тока    там же: UNIT_FGCURRENT = 381470 (381,47 мкА)
 *   калибровка      дерево устройства телефона, узел /battery:
 *                   R_FG_VALUE 10, CAR_TUNE_VALUE 99 — оба
 *                   умножаются драйвером на десять
 */
#define MT6358_FGADC_CON1       0x0D0A      /* защёлка и её готовность   */
#define MT6358_FGADC_CUR_CON0   0x0D8A      /* мгновенный ток, 16 бит    */
#define FG_LATCHDATA_ST         (1U << 15)  /* данные защёлкнуты         */
#define FG_LATCH_REQ            0x0001      /* защёлкнуть                */
#define FG_LATCH_RELEASE        0x0008      /* отпустить                 */

#define FG_UNIT_CURRENT         381470      /* мкА на единицу кода, x1000 */
#define FG_R_VALUE              100         /* R_FG_VALUE 10 x 10         */
#define FG_CAR_TUNE             990         /* CAR_TUNE_VALUE 99 x 10     */

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
    u16 mohm;           /* внутреннее сопротивление в этой точке */
};

static const struct ocv_point curve[] = {
    { 4418, 100, 173 },
    { 4304,  90, 174 },
    { 4182,  80, 171 },
    { 4069,  70, 175 },
    { 3969,  60, 192 },
    { 3867,  50, 174 },
    { 3816,  40, 162 },
    { 3783,  30, 167 },
    { 3745,  20, 178 },
    { 3712,  15, 190 },
    { 3690,  10, 205 },
    { 3667,   7, 230 },
    { 3580,   5, 260 },
    { 3397,   3, 300 },
    { 3192,   2, 905 },
    { 2917,   0, 305 },
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

/* Сопротивление батареи при этом напряжении, миллиомы */
static u32 mv_to_mohm(u32 mv)
{
    const u32 n = sizeof(curve) / sizeof(curve[0]);

    for (u32 i = 0; i < n; i++)
        if (mv >= curve[i].mv)
            return curve[i].mohm;

    return curve[n - 1].mohm;
}

/*
 * Мгновенный ток батареи, миллиамперы. Плюс — заряжается, минус —
 * разряжается. Ноль означает и «нет тока», и «блок не отвечает»: чтобы
 * различить, есть battery_current_valid.
 *
 * Порядок работы — вендорный: защёлкнуть данные, дождаться признака,
 * прочитать, отпустить защёлку. Записи идут ТОЛЬКО в четыре младших
 * бита FGADC_CON1, то есть в саму защёлку; ни настройки счётчика, ни
 * тем более заряд мы не трогаем.
 */
static int fg_current_ma;
static int fg_current_ok;

int battery_current_valid(void) { return fg_current_ok; }

static void fg_wait_latch(int want)
{
    for (u32 i = 0; i < 200; i++) {
        u16 v = 0;

        if (pmic_read(MT6358_FGADC_CON1, &v) != 0)
            return;
        if (!!(v & FG_LATCHDATA_ST) == !!want)
            return;
        udelay(50);
    }
}

static void fg_set_latch(u16 what)
{
    u16 con = 0;

    if (pmic_read(MT6358_FGADC_CON1, &con) != 0)
        return;
    con = (u16)((con & (u16)~0x000F) | what);
    pmic_write(MT6358_FGADC_CON1, con);
}

static int fg_read_current(void)
{
    u16 raw = 0;
    long long mag;
    int charging;

    fg_current_ok = 0;

    fg_set_latch(FG_LATCH_REQ);
    fg_wait_latch(1);

    if (pmic_read(MT6358_FGADC_CUR_CON0, &raw) != 0) {
        fg_set_latch(0);
        return 0;
    }

    fg_set_latch(FG_LATCH_RELEASE);
    fg_wait_latch(0);
    fg_set_latch(0);

    /*
     * Знак спрятан в самом коде: больше половины шкалы — разряд, и
     * величина считается от полной шкалы. Это не дополнительный код и
     * не ошибка чтения, а то, как устроен этот блок.
     */
    if (raw > 32767) {
        mag = 65535 - (long long)raw;
        charging = 0;
    } else {
        mag = raw;
        charging = 1;
    }

    /* Код -> десятые доли миллиампера, затем поправки резистора и
     * калибровки — ровно в том порядке, что у вендора. */
    mag = mag * FG_UNIT_CURRENT / 100000;
    if (FG_R_VALUE != 100)
        mag = mag * 100 / FG_R_VALUE;
    mag = mag * FG_CAR_TUNE / 1000;

    fg_current_ok = 1;
    return charging ? (int)(mag / 10) : -(int)(mag / 10);
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

    /*
     * Честный процент считается не по напряжению на выводах, а по
     * напряжению без нагрузки.
     *
     * Под нагрузкой напряжение проседает на ток, помноженный на
     * внутреннее сопротивление, и процент занижался; под зарядом
     * задирается, и процент завышался. Это было видно наглядно: 12% под
     * нагрузкой превращались в 52% сразу после подключения зарядника —
     * а батарея за секунду не заряжается.
     *
     * Сопротивление берём из вендорного профиля, оно меняется от 173
     * миллиом при полном заряде до трёхсот у пустой батареи.
     */
    fg_current_ma = fg_read_current();
    st->current_ma = fg_current_ma;

    {
        u32 mohm = mv_to_mohm(st->mv);
        int drop = fg_current_ok ? (fg_current_ma * (int)mohm) / 1000 : 0;
        int ocv = (int)st->mv - drop;   /* разряд: ток минусовой, ocv выше */

        if (ocv < 2500)
            ocv = 2500;
        st->ocv_mv = (u32)ocv;
        st->percent = mv_to_percent((u32)ocv);
    }

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
    st->current_ma = 0;
    st->ocv_mv = 0;
}

/* Считать нечем: батареи в эмуляторе нет вовсе */
int battery_current_valid(void) { return 0; }

#endif
