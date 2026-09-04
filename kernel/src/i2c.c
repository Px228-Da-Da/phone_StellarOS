/*
 * Контроллер I2C MediaTek.
 *
 * Карта регистров восстановлена по драйверу Linux i2c-mt65xx.c, раскладка
 * версии 2. Документации на MT6768 нет, поэтому здесь всё написано
 * с расчётом на то, что часть предположений окажется неверной:
 * каждое ожидание ограничено, ни один цикл не может стать бесконечным.
 *
 * Осознанное решение: НЕ делаем программный сброс контроллера. Сброс
 * обнулил бы регистры тактирования шины, которые выставил загрузчик,
 * а восстановить их без документации нечем. Дамп с устройства показал,
 * что LK оставил там осмысленные значения — пользуемся ими как есть
 * и трогаем только то, что относится к самой посылке.
 */
#include "i2c.h"
#include "io.h"

/* --- Регистры (смещения от базы шины) --- */
#define I2C_DATA_PORT       0x00
#define I2C_SLAVE_ADDR      0x04
#define I2C_INTR_MASK       0x08
#define I2C_INTR_STAT       0x0C
#define I2C_CONTROL         0x10
#define I2C_TRANSFER_LEN    0x14
#define I2C_TRANSAC_LEN     0x18
#define I2C_DELAY_LEN       0x1C
#define I2C_TIMING          0x20
#define I2C_START           0x24
#define I2C_EXT_CONF        0x28
#define I2C_TRANSFER_LEN_AUX 0x44   /* длина второй части посылки */
#define I2C_FIFO_ADDR_CLR   0x38
#define I2C_FIFO_STAT       0xF4

/* --- Биты CONTROL --- */
#define CTRL_RS             (1U << 1)   /* повторный старт вместо стопа */
#define CTRL_DMA_EN         (1U << 2)
#define CTRL_CLK_EXT_EN     (1U << 3)
#define CTRL_DIR_CHANGE     (1U << 4)   /* смена направления после записи */
#define CTRL_ACKERR_DET_EN  (1U << 5)   /* сообщать, если устройство молчит */
#define CTRL_TRANSFER_LEN_CHANGE (1U << 6)

/* --- Биты INTR_STAT --- */
#define INTR_TRANSAC_COMP   (1U << 0)   /* посылка завершена            */
#define INTR_ACKERR         (1U << 1)   /* устройство не подтвердило    */
#define INTR_HS_NACKERR     (1U << 2)
#define INTR_ALL            0x7U

/* Сколько ждать завершения. Величина с запасом: восемь байт на 100 кГц
 * идут около миллисекунды, а мы крутим пустой цикл. */
#define I2C_TIMEOUT_SPINS   2000000

static void i2c_prepare(u32 base)
{
    /* Чистим очередь и защёлки прошлой посылки: иначе следующая операция
     * увидит чужой результат и решит, что уже всё готово. */
    mmio_write32(base + I2C_FIFO_ADDR_CLR, 1);
    mmio_write32(base + I2C_INTR_STAT, INTR_ALL);
    mmio_write32(base + I2C_INTR_MASK, 0);      /* прерывания не нужны, опрашиваем */
    dsb();
}

/*
 * Наружу для отладки: без документации на контроллер важно видеть не только
 * «получилось/нет», но и что именно показал регистр статуса и сколько мы
 * ждали. Мгновенное завершение — верный признак того, что мы читаем
 * не тот регистр.
 */
u32 i2c_last_stat;
u32 i2c_last_spins;
u32 i2c_stat_before;

/* Ждём завершения. Возвращает биты статуса или 0 при таймауте. */
static u32 i2c_wait(u32 base)
{
    for (u32 i = 0; i < I2C_TIMEOUT_SPINS; i++) {
        u32 st = mmio_read32(base + I2C_INTR_STAT);

        if (st & (INTR_TRANSAC_COMP | INTR_ACKERR | INTR_HS_NACKERR)) {
            i2c_last_spins = i;
            i2c_last_stat = st;
            return st;
        }
    }
    i2c_last_spins = I2C_TIMEOUT_SPINS;
    i2c_last_stat = 0;
    return 0;
}

u32 i2c_ctrl_orig;                      /* что стояло в регистре до нас */

static int i2c_xfer(u32 base, u8 addr, const u8 *wbuf, u32 wlen,
                    u8 *rbuf, u32 rlen)
{
    /*
     * Свои биты ДОБАВЛЯЕМ к тому, что уже настроил загрузчик, а не пишем
     * регистр целиком. Опыт на устройстве показал, почему это важно: с
     * записью «начисто» контроллер отрабатывал посылку, но переставал
     * сообщать об отсутствии ответа, и пустой адрес выглядел как занятый.
     */
    u32 ctrl = mmio_read32(base + I2C_CONTROL)
             | CTRL_ACKERR_DET_EN | CTRL_CLK_EXT_EN;
    u32 st;

    if (wlen > I2C_MAX_PIO || rlen > I2C_MAX_PIO)
        return -1;                      /* длиннее очереди — нужен DMA */

    i2c_ctrl_orig = mmio_read32(base + I2C_CONTROL);
    i2c_prepare(base);

    if (wlen && rlen) {
        /* Запись, потом чтение без отпускания шины: повторный старт */
        ctrl |= CTRL_RS | CTRL_DIR_CHANGE | CTRL_TRANSFER_LEN_CHANGE;
        mmio_write32(base + I2C_TRANSFER_LEN, wlen);
        mmio_write32(base + I2C_TRANSFER_LEN_AUX, rlen);
        mmio_write32(base + I2C_TRANSAC_LEN, 2);
        mmio_write32(base + I2C_SLAVE_ADDR, (u32)addr << 1);
    } else if (wlen) {
        mmio_write32(base + I2C_TRANSFER_LEN, wlen);
        mmio_write32(base + I2C_TRANSAC_LEN, 1);
        mmio_write32(base + I2C_SLAVE_ADDR, (u32)addr << 1);
    } else {
        mmio_write32(base + I2C_TRANSFER_LEN, rlen);
        mmio_write32(base + I2C_TRANSAC_LEN, 1);
        mmio_write32(base + I2C_SLAVE_ADDR, ((u32)addr << 1) | 1);
    }

    mmio_write32(base + I2C_CONTROL, ctrl);

    /* Данные на запись кладём в очередь ДО старта */
    for (u32 i = 0; i < wlen; i++)
        mmio_write32(base + I2C_DATA_PORT, wbuf[i]);

    dsb();
    i2c_stat_before = mmio_read32(base + I2C_INTR_STAT);
    mmio_write32(base + I2C_START, 1);
    dsb();

    st = i2c_wait(base);
    if (st == 0)
        return -2;                      /* контроллер не ответил вовсе */
    if (st & (INTR_ACKERR | INTR_HS_NACKERR))
        return -3;                      /* устройства по адресу нет     */

    for (u32 i = 0; i < rlen; i++)
        rbuf[i] = (u8)mmio_read32(base + I2C_DATA_PORT);

    mmio_write32(base + I2C_INTR_STAT, INTR_ALL);
    dsb();
    return 0;
}

int i2c_probe(u32 base, u8 addr)
{
    u8 b;

    /* Чтение одного байта: устройство либо подтвердит адрес, либо нет.
     * Что именно прочитается, неважно — нас интересует только ответ. */
    return i2c_xfer(base, addr, 0, 0, &b, 1);
}

int i2c_write(u32 base, u8 addr, const u8 *buf, u32 len)
{
    return i2c_xfer(base, addr, buf, len, 0, 0);
}

int i2c_read(u32 base, u8 addr, u8 *buf, u32 len)
{
    return i2c_xfer(base, addr, 0, 0, buf, len);
}

int i2c_write_read(u32 base, u8 addr, const u8 *wbuf, u32 wlen,
                   u8 *rbuf, u32 rlen)
{
    return i2c_xfer(base, addr, wbuf, wlen, rbuf, rlen);
}
