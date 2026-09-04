/*
 * Выводы общего назначения (GPIO) у MediaTek.
 *
 * Раскладка регистров построена на тройках: по каждому адресу лежит само
 * значение, следом регистр «установить биты», следом «сбросить биты».
 * Смысл в том, чтобы менять один вывод, не читая и не переписывая соседние:
 * запись единицы в SET поднимает только свой бит, остальные не трогает.
 * Это важно, потому что соседними выводами в этот момент может пользоваться
 * загрузчик или другое ядро.
 *
 * Раскладка восстановлена по драйверу Linux pinctrl-mtk-common и проверяется
 * опытом на устройстве: у вывода 1 (прерывание тачскрина) направление обязано
 * быть «вход», у вывода 92 (сброс тачскрина) — «выход». Если это не так,
 * значит смещения угаданы неверно.
 */
#include "gpio.h"
#include "io.h"

#if defined(BOARD_MERLIN)
#include "soc/mt6768.h"

/* Каждая тройка регистров занимает 0x10 и обслуживает 32 вывода */
#define GPIO_DIR_BASE   0x000
#define GPIO_DOUT_BASE  0x100
#define GPIO_DIN_BASE   0x200
#define GPIO_MODE_BASE  0x300

#define REG_VAL         0x0
#define REG_SET         0x4
#define REG_CLR         0x8

static u64 bank_reg(u32 base, u32 pin)
{
    return MT_GPIO_BASE + base + (u64)(pin / 32) * 0x10;
}

/* Режим задаётся четырьмя битами на вывод, то есть восемь выводов на регистр */
static u64 mode_reg(u32 pin)
{
    return MT_GPIO_BASE + GPIO_MODE_BASE + (u64)(pin / 8) * 0x10;
}

int gpio_get_dir(u32 pin)
{
    if (pin >= MT_GPIO_COUNT)
        return -1;
    return (mmio_read32(bank_reg(GPIO_DIR_BASE, pin) + REG_VAL) >> (pin % 32)) & 1;
}

int gpio_get_mode(u32 pin)
{
    if (pin >= MT_GPIO_COUNT)
        return -1;
    return (mmio_read32(mode_reg(pin) + REG_VAL) >> ((pin % 8) * 4)) & 0xF;
}

int gpio_read(u32 pin)
{
    if (pin >= MT_GPIO_COUNT)
        return -1;
    return (mmio_read32(bank_reg(GPIO_DIN_BASE, pin) + REG_VAL) >> (pin % 32)) & 1;
}

int gpio_read_out(u32 pin)
{
    if (pin >= MT_GPIO_COUNT)
        return -1;
    return (mmio_read32(bank_reg(GPIO_DOUT_BASE, pin) + REG_VAL) >> (pin % 32)) & 1;
}

void gpio_set_dir(u32 pin, int dir)
{
    u64 reg = bank_reg(GPIO_DIR_BASE, pin);

    if (pin >= MT_GPIO_COUNT)
        return;
    /* Пишем в SET или CLR, а не в само значение: так соседние выводы,
     * которыми может пользоваться кто-то ещё, остаются нетронутыми. */
    mmio_write32(reg + (dir == GPIO_OUT ? REG_SET : REG_CLR), 1U << (pin % 32));
    dsb();
}

void gpio_write(u32 pin, int value)
{
    u64 reg = bank_reg(GPIO_DOUT_BASE, pin);

    if (pin >= MT_GPIO_COUNT)
        return;
    mmio_write32(reg + (value ? REG_SET : REG_CLR), 1U << (pin % 32));
    dsb();
}

#else   /* в эмуляторе выводов нет */

int  gpio_get_dir(u32 pin)  { (void)pin; return -1; }
int  gpio_get_mode(u32 pin) { (void)pin; return -1; }
int  gpio_read(u32 pin)     { (void)pin; return -1; }
int  gpio_read_out(u32 pin) { (void)pin; return -1; }
void gpio_set_dir(u32 pin, int dir) { (void)pin; (void)dir; }
void gpio_write(u32 pin, int value) { (void)pin; (void)value; }

#endif
