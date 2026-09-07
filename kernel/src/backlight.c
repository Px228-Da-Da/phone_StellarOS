/*
 * Подсветка экрана.
 *
 * Выключателем оказалась микросхема LM3697 на шине i2c0@11007000 по
 * адресу 0x36 — та самая, что названа в дереве устройства узлом
 * lm3697@36. Соседний ktd@37 в дереве тоже описан, но на шине молчит:
 * телефон выпускался с разными панелями, и живой из двух только один.
 *
 * Сюда я пришёл через две ошибки, и обе стоит помнить.
 *
 * Первая: у подсветки в дереве стоит признак mt,use_pwm, а рядом лежит
 * disp_pwm@1100e000 — ШИМ внутри самого чипа. Отсюда следовало, что
 * яркость задаёт он. Железо ответило обратное: EN = 0, блок выключен, а
 * экран горит. Признак в дереве описывает то, что сделал бы драйвер
 * Linux, а не то, что сделал загрузчик. Проверка осталась в коде — она
 * дешёвая, и она же доказывает, что искать надо не там.
 *
 * Вторая: первый вариант этой проверки выбирал раскладку регистров ШИМ
 * по тому, что «похоже на период и скважность», и разрешал скважность
 * ноль. Значение по сбросу 0x3ff это прошло, и система бодро сообщила,
 * что всё опознала. Нулевая скважность — нулевая яркость; при горящем
 * экране такого не бывает. Отсюда правило: признак, который выполняется
 * и на выключенном железе, ничего не доказывает.
 *
 * Карта регистров LM3697 взята из drivers/leds/leds-lm3697.c.
 */
#include "backlight.h"
#include "io.h"
#include "print.h"
#include "usb.h"
#include "i2c.h"
#include "soc/mt6768.h"

#if defined(BOARD_MERLIN)

/* --- Тупик: ШИМ внутри чипа ------------------------------------- */

#define PWM_BASE        0x1100E000UL
#define PWM_EN          0x00
#define PWM_EN_BIT      (1U << 0)

/* --- LM3697 ------------------------------------------------------ */

#define BL_ADDR         0x36

#define LM_REV          0x00
#define LM_RESET        0x01    /* бит 0 — сброс. НИКОГДА не пишем сюда */
#define LM_OUTPUT_CFG   0x10
#define LM_CTRL_A_RAMP  0x11
#define LM_CTRL_B_RAMP  0x12
#define LM_BRT_CFG      0x16
#define LM_A_FS_CURR    0x17
#define LM_B_FS_CURR    0x18
#define LM_PWM_CFG      0x1C
#define LM_A_BRT_LSB    0x20
#define LM_A_BRT_MSB    0x21
#define LM_B_BRT_LSB    0x22
#define LM_B_BRT_MSB    0x23
#define LM_ENABLE       0x24

#define LM_A_EN         (1U << 0)
#define LM_B_EN         (1U << 1)

/* Яркость: одиннадцать бит, три младших в LSB, восемь старших в MSB */
#define BL_MAX          2047
#define BL_MIN          24      /* ниже экран уже не разобрать */

/*
 * Яркость при включении.
 *
 * Загрузчик оставляет ровно максимум — те самые 2047, — и до сих пор мы
 * их не трогали. Это и есть источник тепла у нижнего края экрана: там
 * идёт полоса светодиодов, и на полной мощности она греет заметно.
 * Половина — та величина, с которой телефон и виден, и не горячий.
 */
#define BL_DEFAULT      50

static u8  saved_enable;        /* что стояло в 0x24 при горящем экране */
static u32 bl_percent = 100;
static int have_bl;             /* нашли и поняли микросхему */
static int probed;

static int bl_read(u8 reg, u8 *val)
{
    return i2c_write_read(MT_I2C0_BASE, BL_ADDR, &reg, 1, val, 1);
}

static int bl_write(u8 reg, u8 val)
{
    u8 buf[2] = { reg, val };

    return i2c_write(MT_I2C0_BASE, BL_ADDR, buf, 2);
}

/*
 * Проверка ШИМ внутри чипа. Оставлена как свидетельство, а не как путь:
 * одна строка в журнале вместо прежних тринадцати.
 */
static void pwm_check(void)
{
    u32 en;

    kprintf("ПОДСВЕТКА: ЧИТАЮ disp_pwm ПО 0x%08lx...\n", PWM_BASE);
    usb_flush();

    en = mmio_read32(PWM_BASE + PWM_EN);
    kprintf("ПОДСВЕТКА: disp_pwm EN %08x, CON %08x/%08x — %s\n",
            en, mmio_read32(PWM_BASE + 0x18), mmio_read32(PWM_BASE + 0x1C),
            (en & PWM_EN_BIT) ? "РАБОТАЕТ" : "ВЫКЛЮЧЕН, ЯРКОСТЬ НЕ ОТСЮДА");
    usb_flush();
}

void backlight_probe(void)
{
    u8 rev, enable, cfg, msb_a, msb_b, lsb_a, lsb_b, cur_a, cur_b;

    if (probed)
        return;
    probed = 1;

    pwm_check();

    if (i2c_probe(MT_I2C0_BASE, BL_ADDR) != 0) {
        kprintf("ПОДСВЕТКА: ПО %02x НИКОГО, УПРАВЛЕНИЯ НЕТ\n", BL_ADDR);
        usb_flush();
        return;
    }

    if (bl_read(LM_REV, &rev) || bl_read(LM_ENABLE, &enable) ||
        bl_read(LM_OUTPUT_CFG, &cfg) ||
        bl_read(LM_A_BRT_MSB, &msb_a) || bl_read(LM_A_BRT_LSB, &lsb_a) ||
        bl_read(LM_B_BRT_MSB, &msb_b) || bl_read(LM_B_BRT_LSB, &lsb_b) ||
        bl_read(LM_A_FS_CURR, &cur_a) || bl_read(LM_B_FS_CURR, &cur_b)) {
        kprintf("ПОДСВЕТКА: ПО %02x ОТВЕТИЛ, НО ЧИТАТЬ НЕ ДАЁТ\n", BL_ADDR);
        usb_flush();
        return;
    }

    kprintf("ПОДСВЕТКА: lm3697 ВЕРСИЯ %02x, ВКЛЮЧЕНО %02x, ВЫХОДЫ %02x\n",
            rev, enable, cfg);
    kprintf("ПОДСВЕТКА: ЯРКОСТЬ A %02x%02x ТОК %02x, B %02x%02x ТОК %02x\n",
            msb_a, lsb_a, cur_a, msb_b, lsb_b, cur_b);
    usb_flush();

    /*
     * Признак, которого НЕ может быть на выключенной микросхеме: хотя бы
     * один канал разрешён И у него ненулевая яркость. После сброса в
     * регистре разрешения ноль, так что совпасть случайно это не может —
     * в отличие от прошлой проверки, на которой я и обжёгся.
     */
    if (!(enable & (LM_A_EN | LM_B_EN))) {
        kprintf("ПОДСВЕТКА: КАНАЛЫ ЗАПРЕЩЕНЫ, А ЭКРАН ГОРИТ — НЕ ПОНЯЛ, "
                "НЕ ТРОГАЮ\n");
        usb_flush();
        return;
    }
    if (((enable & LM_A_EN) && !msb_a && !lsb_a) ||
        ((enable & LM_B_EN) && !msb_b && !lsb_b)) {
        kprintf("ПОДСВЕТКА: РАЗРЕШЁННЫЙ КАНАЛ С НУЛЕВОЙ ЯРКОСТЬЮ — "
                "НЕ ПОНЯЛ, НЕ ТРОГАЮ\n");
        usb_flush();
        return;
    }

    saved_enable = enable;
    have_bl = 1;
    kprintf("ПОДСВЕТКА: УПРАВЛЕНИЕ ЕСТЬ, ВЕРНУ %02x\n", saved_enable);

    if (backlight_level(BL_DEFAULT) == 0)
        kprintf("ПОДСВЕТКА: ЯРКОСТЬ %u%% (БЫЛА ПОЛНАЯ)\n", BL_DEFAULT);
    else
        kprintf("ПОДСВЕТКА: ЯРКОСТЬ СМЕНИТЬ НЕ ВЫШЛО\n");
    usb_flush();
}

int backlight_known(void)
{
    return have_bl;
}

/*
 * Задать яркость, 0..100 процентов. Возвращает 0, если получилось.
 *
 * Яркость у LM3697 одиннадцатибитная и разложена на два регистра: три
 * младших бита в LSB, восемь старших в MSB. Порядок записи не
 * произвольный — сперва LSB, потом MSB, как в drivers/leds/leds-ti-lmu-
 * common.c: микросхема принимает новое значение по записи в MSB, и если
 * писать наоборот, между двумя записями окажется яркость с новым верхом
 * и старым низом. На глаз это дёрганье при плавном изменении.
 *
 * В LSB трогаем только три младших бита: остальные там не наши.
 *
 * Полностью в ноль не уводим. Ноль это чёрный экран, неотличимый от
 * сломанного, и вернуть его тогда можно будет только на ощупь. Для
 * «совсем выключить» есть backlight_set.
 */
int backlight_level(u32 percent)
{
    u32 level;
    u8 lsb;

    if (!have_bl)
        return -1;
    if (percent > 100)
        percent = 100;

    level = percent * BL_MAX / 100;
    if (level < BL_MIN)
        level = BL_MIN;

    for (u32 bank = 0; bank < 2; bank++) {
        if (!(saved_enable & (1U << bank)))
            continue;

        if (bl_read((u8)(LM_A_BRT_LSB + bank * 2), &lsb))
            return -1;
        lsb = (u8)((lsb & ~0x07u) | (level & 0x07u));

        if (bl_write((u8)(LM_A_BRT_LSB + bank * 2), lsb) ||
            bl_write((u8)(LM_A_BRT_MSB + bank * 2), (u8)(level >> 3)))
            return -1;
    }

    bl_percent = percent;
    return 0;
}

int backlight_percent(void)
{
    return have_bl ? (int)bl_percent : -1;
}

/*
 * Включить или выключить подсветку.
 *
 * Гасим регистром разрешения каналов, а не яркостью. Разница не
 * косметическая: при нулевой яркости повышающий преобразователь внутри
 * микросхемы продолжает работать вхолостую, а запрет каналов
 * останавливает и его. Прежнее значение возвращаем целиком — вместе с
 * ним возвращается и та яркость, что была, потому что регистры яркости
 * мы не трогали вовсе.
 *
 * В регистр 0x01 не пишем никогда: его нулевой бит — сброс микросхемы.
 */
void backlight_set(int on)
{
    if (!have_bl)
        return;

    bl_write(LM_ENABLE, on ? saved_enable : 0);
}

#else

void backlight_probe(void) { }
int  backlight_known(void) { return 0; }
void backlight_set(int on) { (void)on; }
int  backlight_level(u32 percent) { (void)percent; return -1; }
int  backlight_percent(void) { return -1; }

#endif
