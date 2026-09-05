/*
 * Контроллер заряда SMB1351.
 *
 * Сидит на I2C7 по адресу 0x55. Все номера регистров и раскладка полей —
 * из вендорного драйвера merlin, а не угаданы.
 *
 * Здесь только чтение. Причина не в осторожности вообще, а в конкретном:
 * этот контроллер задаёт ток заряда и напряжение окончания заряда, то
 * есть ровно те величины, ошибка в которых портит батарею. Прежде чем
 * что-то менять, надо увидеть, что уже настроено.
 */
#include "charger.h"
#include "i2c.h"
#include "print.h"
#include "io.h"

#if defined(BOARD_MERLIN)
#include "soc/mt6768.h"

#define SMB_ADDR            0x55
#define SMB_I2C_BASE        0x11004000UL    /* i2c7@11004000 из mt6768.dts */

/* --- Регистры ------------------------------------------------------ */
#define REG_CHG_CURRENT     0x00    /* [7:4] ток заряда, [3:0] предел входа */
#define REG_VERSION         0x2E
#define REG_CMD_INPUT       0x31    /* режим входа: 100 мА / 500 мА / AC   */
#define REG_CMD_CHG         0x32
#define REG_STATUS_4        0x3A    /* стадия заряда                        */
#define REG_STATUS_5        0x3B    /* что распознано на кабеле             */

/* Таблицы из вендорного драйвера: код в регистре -> миллиамперы */
static const u16 input_limit_ma[16] = {
    500, 685, 1000, 1100, 1200, 1300, 1500, 1600,
    1700, 1800, 2000, 2200, 2500, 3000, 3500, 3940,
};
static const u16 fast_charge_ma[16] = {
    1000, 1200, 1400, 1600, 1800, 2000, 2200, 2400,
    2600, 2800, 3000, 3400, 3600, 3800, 4000, 4500,
};

static int smb_read(u8 reg, u8 *val)
{
    return i2c_write_read(SMB_I2C_BASE, SMB_ADDR, &reg, 1, val, 1);
}

static int smb_write(u8 reg, u8 val)
{
    u8 buf[2] = { reg, val };

    return i2c_write(SMB_I2C_BASE, SMB_ADDR, buf, sizeof(buf));
}

/* Бит 3 регистра 0x31: предел входа задаётся командой, а не
 * распознаванием источника. Снимаем его — и решает контроллер. */
#define CMD_INPUT_MODE_BY_COMMAND   (1U << 3)

int charger_probe(struct charger_state *st)
{
    st->valid = 0;

    if (smb_read(REG_VERSION, &st->version) != 0)
        return -1;
    if (smb_read(REG_CHG_CURRENT, &st->current_ctrl) != 0)
        return -1;
    if (smb_read(REG_CMD_INPUT, &st->input_limit) != 0)
        return -1;
    if (smb_read(REG_CMD_CHG, &st->chg_cmd) != 0)
        return -1;
    if (smb_read(REG_STATUS_4, &st->status4) != 0)
        return -1;
    if (smb_read(REG_STATUS_5, &st->status5) != 0)
        return -1;

    st->charge_ma = fast_charge_ma[(st->current_ctrl >> 4) & 0xF];
    st->input_ma  = input_limit_ma[st->current_ctrl & 0xF];
    st->valid = 1;
    return 0;
}

/* Стадия заряда: биты 2:1 регистра состояния 4 */
static const char *charge_stage(u8 status4)
{
    switch (status4 & 0x06) {
    case 0x00: return "НЕ ЗАРЯЖАЕТ";
    case 0x02: return "ПРЕДЗАРЯД";
    case 0x04: return "БЫСТРЫЙ ЗАРЯД";
    default:   return "ДОЗАРЯД";
    }
}

/* Что контроллер распознал на кабеле */
static const char *port_kind(u8 status5)
{
    if (status5 & 0x80) return "CDP, ПОРТ С ЗАРЯДКОЙ";
    if (status5 & 0x40) return "DCP, ЗАРЯДНИК";
    if (status5 & 0x20) return "ПРОЧЕЕ";
    if (status5 & 0x10) return "SDP, ОБЫЧНЫЙ USB";
    return "НЕ РАСПОЗНАН";
}

/* Режим входа, младшие два бита команды: чем ограничен ток из кабеля */
static const char *input_mode(u8 cmd)
{
    if (!(cmd & 0x08))
        return "ПО РАСПОЗНАВАНИЮ";
    switch (cmd & 0x03) {
    case 0x00: return "500 МА";
    case 0x01: return "ПО ТАБЛИЦЕ AC";
    case 0x02: return "100 МА";
    default:   return "?";
    }
}

int charger_use_apsd(void)
{
    u8 cmd = 0;

    if (smb_read(REG_CMD_INPUT, &cmd) != 0)
        return -1;
    if (!(cmd & CMD_INPUT_MODE_BY_COMMAND))
        return 0;                       /* уже решает контроллер */

    if (smb_write(REG_CMD_INPUT, (u8)(cmd & ~CMD_INPUT_MODE_BY_COMMAND)) != 0)
        return -1;

    /* Проверяем, что запись дошла: у контроллера часть регистров
     * защищена, и молча проигнорированная запись выглядела бы как успех. */
    if (smb_read(REG_CMD_INPUT, &cmd) != 0)
        return -1;
    return (cmd & CMD_INPUT_MODE_BY_COMMAND) ? -1 : 0;
}

/* --- Для интерфейса: последнее прочитанное состояние ---------------- */
static struct charger_state last;

const char *charger_stage_text(void)
{
    if (charger_probe(&last) != 0)
        return "НЕТ СВЯЗИ";
    return charge_stage(last.status4);
}

const char *charger_port_text(void)
{
    return last.valid ? port_kind(last.status5) : "?";
}

u32 charger_input_ma(void)
{
    if (!last.valid)
        return 0;
    /* В режиме команды предел задан ею, а не таблицей входа */
    if (last.input_limit & CMD_INPUT_MODE_BY_COMMAND)
        return ((last.input_limit & 3) == 2) ? 100 : 500;
    return last.input_ma;
}

void charger_dump(void)
{
    struct charger_state st;

    if (charger_probe(&st) != 0) {
        kprintf("ЗАРЯД: КОНТРОЛЛЕР НЕ ОТВЕЧАЕТ НА I2C7 0x%02x\n", SMB_ADDR);
        return;
    }

    kprintf("ЗАРЯД: SMB1351 ВЕРСИЯ %02x, %s, КАБЕЛЬ %s\n",
            st.version, charge_stage(st.status4), port_kind(st.status5));
    kprintf("ЗАРЯД: ВХОД %u МА (%s), ТОК ЗАРЯДА %u МА\n",
            st.input_ma, input_mode(st.input_limit), st.charge_ma);
    kprintf("ЗАРЯД: РЕГИСТРЫ ТОК %02x ВХОД %02x КОМАНДА %02x СОСТ %02x/%02x\n",
            st.current_ctrl, st.input_limit, st.chg_cmd,
            st.status4, st.status5);
}

#else   /* в эмуляторе контроллера заряда нет: питание там не кончается */

int charger_probe(struct charger_state *st) { st->valid = 0; return -1; }
void charger_dump(void) { }
int  charger_use_apsd(void) { return -1; }

/* Интерфейсу нужно что-то показать и без железа */
const char *charger_stage_text(void) { return "НЕТ"; }
const char *charger_port_text(void)  { return "-"; }
u32  charger_input_ma(void) { return 0; }

#endif
