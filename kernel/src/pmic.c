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

#if defined(BOARD_MERLIN)
#include "soc/mt6768.h"

#define PWRAP_BASE          0x1000D000UL

/* Состояние конечного автомата лежит в битах 18..16 ответа */
#define PWRAP_FSM(x)        (((x) >> 16) & 0x7)
#define FSM_IDLE            0x00
#define FSM_WFVLDCLR        0x06        /* ответ готов, ждём подтверждения */

static u32 pwrap_spins = 200000;

void pmic_set_spins(u32 n) { pwrap_spins = n; }

/*
 * Найти смещения регистров перебором.
 *
 * Гадать по драйверу Linux оказалось бесполезно: у MT6768 раскладка не
 * совпала ни с одним из двух ожидаемых вариантов. Но блок PWRAP занимает
 * всего 4 КБ, то есть 1024 варианта по четыре байта — их можно просто
 * перебрать и спросить у PMIC его идентификатор. Настоящие регистры
 * ответят, пустые промолчат.
 *
 * Тройка регистров идёт подряд (команда, ответ, подтверждение) во всех
 * поколениях, поэтому перебираем только начало тройки.
 *
 * Возвращает найденное смещение команды или 0 при неудаче.
 */
u32 pmic_find_regs(u16 *id_out)
{
    u32 saved = pwrap_spins;

    pwrap_spins = 4000;                 /* при переборе ждать долго незачем */
    for (u32 off = 0; off + 8 < 0x1000; off += 4) {
        u16 id = 0;

        pmic_use_regs(off, off + 4, off + 8);
        if (pmic_read(MT6358_SWCID, &id) == 0 && id != 0 && id != 0xFFFF) {
            pwrap_spins = saved;
            if (id_out)
                *id_out = id;
            return off;
        }
    }
    pwrap_spins = saved;
    return 0;
}


/* Поколение mt6765/mt6768 — значение по умолчанию */
static u32 reg_cmd    = 0xC80;
static u32 reg_rdata  = 0xC84;
static u32 reg_vldclr = 0xC88;

void pmic_use_regs(u32 cmd, u32 rdata, u32 vldclr)
{
    reg_cmd = cmd;
    reg_rdata = rdata;
    reg_vldclr = vldclr;
}

static int wait_fsm(u32 want)
{
    for (u32 i = 0; i < pwrap_spins; i++) {
        if (PWRAP_FSM(mmio_read32(PWRAP_BASE + reg_rdata)) == want)
            return 0;
    }
    return -1;
}

int pmic_read(u32 reg, u16 *out)
{
    u32 r;

    if (wait_fsm(FSM_IDLE) != 0)
        return -1;                      /* обёртка не в исходном состоянии */

    /* Старший бит нулевой — это чтение */
    mmio_write32(PWRAP_BASE + reg_cmd, (reg >> 1) << 16);
    dsb();

    if (wait_fsm(FSM_WFVLDCLR) != 0)
        return -2;                      /* ответа не дождались */

    r = mmio_read32(PWRAP_BASE + reg_rdata);
    if (out)
        *out = (u16)(r & 0xFFFF);

    /* Подтверждаем приём, иначе следующая команда не пойдёт */
    mmio_write32(PWRAP_BASE + reg_vldclr, 1);
    dsb();
    return 0;
}

int pmic_write(u32 reg, u16 val)
{
    if (wait_fsm(FSM_IDLE) != 0)
        return -1;

    /* Старший бит единица — это запись */
    mmio_write32(PWRAP_BASE + reg_cmd,
                 (1U << 31) | ((reg >> 1) << 16) | val);
    dsb();
    return 0;
}

#else   /* в эмуляторе PMIC нет */

void pmic_use_regs(u32 c, u32 r, u32 v) { (void)c; (void)r; (void)v; }
int  pmic_read(u32 reg, u16 *out) { (void)reg; (void)out; return -1; }
int  pmic_write(u32 reg, u16 val)  { (void)reg; (void)val; return -1; }

#endif
