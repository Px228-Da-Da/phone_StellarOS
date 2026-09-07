/*
 * PMIC MT6358 через обёртку PWRAP.
 *
 * Обмен устроен так: в регистр команды кладётся адрес регистра PMIC (и, для
 * записи, данные), аппаратура сама прогоняет посылку по последовательной шине
 * и переводит свой конечный автомат в состояние «ответ готов». Мы дожидаемся
 * этого состояния, забираем данные и подтверждаем приём — иначе следующая
 * команда не пойдёт.
 *
 * Адрес регистра PMIC делится пополам перед отправкой: шина адресует
 * шестнадцатибитные слова, а не байты. Это не наша выдумка, так устроен
 * протокол (см. mtk-pmic-wrap.c).
 *
 * Смещения самих регистров PWRAP у разных поколений MediaTek различаются,
 * поэтому они не зашиты намертво: pmic_use_regs() позволяет перебрать
 * варианты и найти рабочий по ответу PMIC.
 */
#include "pmic.h"
#include "io.h"
#include "spinlock.h"

#if defined(BOARD_MERLIN)
#include "soc/mt6768.h"

#define PWRAP_BASE          0x1000D000UL

/*
 * Раскладка снята с живого устройства дампом всего блока.
 *
 * Регистры идут группами по 0x10 — это четыре независимых канала связи
 * с PMIC, у каждого своя тройка «команда / ответ / подтверждение».
 * Загрузчик пользуется третьим (0xC20): там осталась его последняя
 * команда 0x02DD0000 и ответ 0x80F00800.
 *
 * Готовность отмечается СТАРШИМ БИТОМ ответа, а не полем состояния
 * автомата, как я предполагал сначала. Именно поэтому перебор смещений
 * ничего не находил: он ждал признака, которого здесь нет.
 */
#define PWRAP_WACS2_CMD     0xC20
#define PWRAP_WACS2_RDATA   0xC24
#define PWRAP_WACS2_VLDCLR  0xC28

#define PWRAP_VALID         (1U << 31)  /* ответ готов */

/*
 * Замок на всю шину PMIC.
 *
 * У контроллера одна пара регистров: в один кладут команду, из другого
 * забирают ответ. Две команды, начатые одновременно с разных ядер,
 * перемешиваются, и второе ядро забирает ответ на чужой запрос. Как это
 * выглядит снаружи, написано ниже в pmic_read: ответ отстаёт ровно на
 * один запрос, и вместо своего значения приходит предыдущее.
 *
 * Долгое время сюда ходила одна задача питания, и обходилось. Замок
 * понадобился, когда часы на экране блокировки стали спрашивать время
 * по нескольку раз в секунду — уже с другого ядра и вперемешку с
 * замерами батареи.
 *
 * Прерывания на время обмена закрыты: внутри есть ожидание по счётчику
 * времени, и вытеснение посреди транзакции оставило бы шину занятой
 * чужой командой.
 */
static struct spinlock pmic_lock;

static u32 pwrap_spins = 200000;

void pmic_set_spins(u32 n) { pwrap_spins = n; }

/* Смещения оставлены переменными: пригодилось при поиске раскладки,
 * и пусть остаются — вдруг у другого экземпляра канал окажется иным. */
static u32 reg_cmd    = PWRAP_WACS2_CMD;
static u32 reg_rdata  = PWRAP_WACS2_RDATA;
static u32 reg_vldclr = PWRAP_WACS2_VLDCLR;

void pmic_use_regs(u32 cmd, u32 rdata, u32 vldclr)
{
    reg_cmd = cmd;
    reg_rdata = rdata;
    reg_vldclr = vldclr;
}

static int pmic_read_locked(u32 reg, u16 *out)
{
    u64 wait;

    /*
     * Ответ забираем не сразу, а выждав паузу.
     *
     * Сначала я ждал признака готовности в старшем бите ответа — и попался:
     * этот бит стоит всегда, очистка его не сбрасывает, поэтому чтение
     * происходило мгновенно и возвращало результат ПРЕДЫДУЩЕЙ команды.
     * Опознали по характерному признаку: каждый ответ отставал ровно на
     * один запрос, и вместо идентификатора PMIC приходил остаток команды
     * загрузчика.
     *
     * Обмен по шине PMIC занимает единицы микросекунд; ждём пятьдесят —
     * с запасом, но всё ещё незаметно.
     */
    mmio_write32(PWRAP_BASE + reg_vldclr, 1);
    dsb();

    /* Старший бит нулевой — это чтение. Адрес делится пополам: шина
     * адресует шестнадцатибитные слова, а не байты. */
    mmio_write32(PWRAP_BASE + reg_cmd, (reg >> 1) << 16);
    dsb();

    wait = read_cntpct() + read_cntfrq() / 20000;    /* 50 мкс */
    while (read_cntpct() < wait)
        ;

    if (out)
        *out = (u16)(mmio_read32(PWRAP_BASE + reg_rdata) & 0xFFFF);

    mmio_write32(PWRAP_BASE + reg_vldclr, 1);
    dsb();
    return 0;
}

static int pmic_write_locked(u32 reg, u16 val)
{
    u64 wait;

    mmio_write32(PWRAP_BASE + reg_vldclr, 1);
    dsb();
    /* Старший бит единица — это запись */
    mmio_write32(PWRAP_BASE + reg_cmd, (1U << 31) | ((reg >> 1) << 16) | val);
    dsb();

    /* Даём посылке уйти по шине, иначе следующая команда затрёт эту */
    wait = read_cntpct() + read_cntfrq() / 20000;
    while (read_cntpct() < wait)
        ;
    return 0;
}

/*
 * Поиск канала перебором остаётся про запас: раскладку мы теперь знаем,
 * но если у другого экземпляра канал окажется иным, перебор её найдёт.
 */
u32 pmic_find_regs(u16 *id_out)
{
    u32 saved = pwrap_spins;

    pwrap_spins = 4000;
    for (u32 off = 0; off + 8 < 0x1000; off += 4) {
        u16 id = 0;

        pmic_use_regs(off, off + 4, off + 8);
        if (pmic_read_locked(MT6358_SWCID, &id) == 0 && id != 0 && id != 0xFFFF) {
            pwrap_spins = saved;
            if (id_out)
                *id_out = id;
            return off;
        }
    }
    pmic_use_regs(PWRAP_WACS2_CMD, PWRAP_WACS2_RDATA, PWRAP_WACS2_VLDCLR);
    pwrap_spins = saved;
    return 0;
}

/*
 * Наружу выходят обёртки, берущие замок.
 *
 * Разделено на две части, потому что перебор каналов в pmic_find_regs
 * ходит к шине сам: если бы обмен всегда брал замок, а перебор вызывал
 * обмен, взять его пришлось бы дважды. Внутренние функции замка не
 * трогают, внешние берут — и перебор пользуется внутренними.
 */
int pmic_read(u32 reg, u16 *out)
{
    u64 flags = spin_lock_irq(&pmic_lock);
    int rc = pmic_read_locked(reg, out);

    spin_unlock_irq(&pmic_lock, flags);
    return rc;
}

int pmic_write(u32 reg, u16 val)
{
    u64 flags = spin_lock_irq(&pmic_lock);
    int rc = pmic_write_locked(reg, val);

    spin_unlock_irq(&pmic_lock, flags);
    return rc;
}

/*
 * Кнопка питания.
 *
 * Живёт в том же регистре состояния, что и признак зарядки, но в другом
 * бите. Раскладка из драйвера вендора drivers/input/keyboard/mtk-pmic-keys.c:
 * для MT6358 кнопка питания это маска 0x2 в MT6358_TOPSTATUS.
 *
 * Внимание к знаку: там же видно, что нажатие это НОЛЬ, а не единица —
 * «pressed = !key_deb». Регистр показывает не «нажата», а «отпущена»,
 * и перепутать здесь означало бы получить кнопку, нажатую всегда.
 */
#define MT6358_TOPSTATUS_REG    0x0028
#define PWRKEY_MASK             0x0002

int pmic_powerkey(void)
{
    u16 v;

    if (pmic_read(MT6358_TOPSTATUS_REG, &v) != 0)
        return 0;
    return (v & PWRKEY_MASK) ? 0 : 1;
}

#else   /* в эмуляторе PMIC нет */

void pmic_use_regs(u32 c, u32 r, u32 v) { (void)c; (void)r; (void)v; }
int  pmic_read(u32 reg, u16 *out) { (void)reg; (void)out; return -1; }
int  pmic_write(u32 reg, u16 val)  { (void)reg; (void)val; return -1; }
int  pmic_powerkey(void) { return 0; }

#endif
