/*
 * Тачскрин Novatek NT36xxx: опознание микросхемы.
 *
 * Всё, что здесь есть, списано с драйвера Xiaomi для merlin, а не выведено
 * опытом. Причина простая: одна попытка на телефоне стоит перепрошивки и
 * перезагрузки, а вариантов у протокола слишком много, чтобы их перебирать.
 *
 * Как устроен обмен:
 *   первый байт посылки — адрес внутри страницы;
 *   старший бит адреса задаёт направление: 1 — запись, 0 — чтение;
 *   при чтении контроллер отдаёт данные с задержкой в один байт, поэтому
 *   на шину уходит на байт больше, а полезное начинается с третьего;
 *   страницу задаёт отдельная команда 0xFF с двумя байтами адреса.
 */
#include "nvt.h"
#include "spi.h"
#include "gpio.h"
#include "print.h"
#include "io.h"

#if defined(BOARD_MERLIN)
#include "soc/mt6768.h"
#include "usb.h"

/* Адреса из узла novatek@0 дерева устройств merlin */
#define ENG_RST_ADDR        0x7FFF80    /* «инженерный» сброс, общий для всех */
#define SWRST_N8_ADDR       0x03F0FE    /* novatek,swrst-n8-addr */
#define SPI_RD_FAST_ADDR    0x03F310    /* novatek,spi-rd-fast-addr */

/* Где лежит идентификатор микросхемы */
#define CHIP_VER_TRIM_ADDR  0x1F64E

static void mdelay(u32 ms)
{
    u64 target = read_cntpct() + (read_cntfrq() / 1000) * ms;

    while (read_cntpct() < target)
        usb_poll();     /* иначе за время паузы хост объявит порт мёртвым */
}

int nvt_write(const u8 *buf, u32 len)
{
    u8 tx[SPI_FIFO_MAX];

    if (!len || len > SPI_FIFO_MAX)
        return -1;
    for (u32 i = 0; i < len; i++)
        tx[i] = buf[i];
    tx[0] = buf[0] | 0x80;              /* старший бит адреса: запись */
    return spi_transfer(tx, 0, len);
}

int nvt_read(u8 *buf, u32 len)
{
    u8 tx[SPI_FIFO_MAX], rx[SPI_FIFO_MAX];
    int r;

    if (len < 2 || len + 1 > SPI_FIFO_MAX)
        return -1;

    for (u32 i = 0; i < len + 1; i++)
        tx[i] = 0;
    tx[0] = buf[0] & 0x7F;              /* старший бит снят: чтение */

    r = spi_transfer(tx, rx, len + 1);
    if (r)
        return r;

    /* Первые два принятых байта — эхо адреса и холостой такт */
    for (u32 i = 1; i < len; i++)
        buf[i] = rx[i + 1];
    return 0;
}

int nvt_set_page(u32 addr)
{
    u8 buf[3];

    buf[0] = 0xFF;                      /* команда «задать страницу» */
    buf[1] = (addr >> 15) & 0xFF;
    buf[2] = (addr >> 7) & 0xFF;
    return nvt_write(buf, 3);
}

int nvt_write_addr(u32 addr, u8 data)
{
    u8 buf[2];
    int r = nvt_set_page(addr);

    if (r)
        return r;
    buf[0] = addr & 0x7F;
    buf[1] = data;
    return nvt_write(buf, 2);
}

/* Сброс до состояния загрузчика микросхемы. Ровно то, что делает драйвер
 * перед каждой попыткой прочитать идентификатор. */
static void nvt_bootloader_reset(void)
{
    nvt_write_addr(SWRST_N8_ADDR, 0x69);
    mdelay(5);
    /* Быстрое чтение по SPI выключаем: пока мы к нему не готовы */
    nvt_write_addr(SPI_RD_FAST_ADDR, 0x00);
}

/*
 * Аппаратный сброс линией 92.
 *
 * Драйвер поднимает эту линию и ждёт 10 мс — столько микросхеме нужно на
 * собственный сброс по питанию. Питание при этом уже есть: панель подняли
 * загрузчик и мы сами, тачскрин сидит на тех же цепях.
 */
static void nvt_hw_reset(void)
{
    gpio_set_mode(NVT_GPIO_RESET, 0);           /* обычный вывод */
    gpio_set_dir(NVT_GPIO_RESET, GPIO_OUT);
    gpio_write(NVT_GPIO_RESET, 0);
    mdelay(10);
    gpio_write(NVT_GPIO_RESET, 1);
    mdelay(10);

    /* Линия прерывания — вход: по ней микросхема сообщает о касании */
    gpio_set_mode(NVT_GPIO_IRQ, 0);
    gpio_set_dir(NVT_GPIO_IRQ, GPIO_IN);
}

/*
 * Таблица идентификаторов из nt36xxx_mem_map.h.
 *
 * Сравниваются не все байты: маска показывает, какие значимы. Средние два
 * байта — версия кристалла, они у одной и той же микросхемы разные.
 */
struct nvt_id {
    u8          id[6];
    u8          mask[6];
    const char *name;
};

static const struct nvt_id nvt_ids[] = {
    {{0x0D, 0xFF, 0xFF, 0x72, 0x66, 0x03}, {1, 0, 0, 1, 1, 1}, "NT36675"},
    {{0x0C, 0xFF, 0xFF, 0x72, 0x66, 0x03}, {1, 0, 0, 1, 1, 1}, "NT36675"},
    {{0xFF, 0xFF, 0xFF, 0x26, 0x65, 0x03}, {0, 0, 0, 1, 1, 1}, "NT36526"},
    {{0xFF, 0xFF, 0xFF, 0x75, 0x66, 0x03}, {0, 0, 0, 1, 1, 1}, "NT36675"},
    {{0x0B, 0xFF, 0xFF, 0x72, 0x66, 0x03}, {1, 0, 0, 1, 1, 1}, "NT36672A"},
    {{0x0B, 0xFF, 0xFF, 0x82, 0x66, 0x03}, {1, 0, 0, 1, 1, 1}, "NT36672A"},
    {{0x0B, 0xFF, 0xFF, 0x25, 0x65, 0x03}, {1, 0, 0, 1, 1, 1}, "NT36672A"},
    {{0x0A, 0xFF, 0xFF, 0x72, 0x65, 0x03}, {1, 0, 0, 1, 1, 1}, "NT36672A"},
    {{0x0A, 0xFF, 0xFF, 0x72, 0x66, 0x03}, {1, 0, 0, 1, 1, 1}, "NT36672A"},
    {{0x0A, 0xFF, 0xFF, 0x82, 0x66, 0x03}, {1, 0, 0, 1, 1, 1}, "NT36672A"},
    {{0x0A, 0xFF, 0xFF, 0x70, 0x66, 0x03}, {1, 0, 0, 1, 1, 1}, "NT36672A"},
    {{0x0B, 0xFF, 0xFF, 0x70, 0x66, 0x03}, {1, 0, 0, 1, 1, 1}, "NT36672A"},
    {{0x0A, 0xFF, 0xFF, 0x72, 0x67, 0x03}, {1, 0, 0, 1, 1, 1}, "NT36672A"},
    {{0x55, 0x00, 0xFF, 0x00, 0x00, 0x00}, {1, 1, 0, 1, 1, 1}, "NT36772"},
    {{0x55, 0x72, 0xFF, 0x00, 0x00, 0x00}, {1, 1, 0, 1, 1, 1}, "NT36772"},
    {{0xAA, 0x00, 0xFF, 0x00, 0x00, 0x00}, {1, 1, 0, 1, 1, 1}, "NT36772"},
    {{0xAA, 0x72, 0xFF, 0x00, 0x00, 0x00}, {1, 1, 0, 1, 1, 1}, "NT36772"},
    {{0xFF, 0xFF, 0xFF, 0x72, 0x67, 0x03}, {0, 0, 0, 1, 1, 1}, "NT36772"},
    {{0xFF, 0xFF, 0xFF, 0x70, 0x66, 0x03}, {0, 0, 0, 1, 1, 1}, "NT36772"},
    {{0xFF, 0xFF, 0xFF, 0x70, 0x67, 0x03}, {0, 0, 0, 1, 1, 1}, "NT36772"},
    {{0xFF, 0xFF, 0xFF, 0x72, 0x66, 0x03}, {0, 0, 0, 1, 1, 1}, "NT36772"},
    {{0xFF, 0xFF, 0xFF, 0x25, 0x65, 0x03}, {0, 0, 0, 1, 1, 1}, "NT36525"},
    {{0xFF, 0xFF, 0xFF, 0x76, 0x66, 0x03}, {0, 0, 0, 1, 1, 1}, "NT36676F"},
};

static const char *nvt_match(const u8 *got)
{
    for (u32 n = 0; n < sizeof(nvt_ids) / sizeof(nvt_ids[0]); n++) {
        u32 i;

        for (i = 0; i < 6; i++)
            if (nvt_ids[n].mask[i] && got[i] != nvt_ids[n].id[i])
                break;
        if (i == 6)
            return nvt_ids[n].name;
    }
    return 0;
}

void nvt_probe(void)
{
    u8 buf[8];

    nvt_hw_reset();

    kprintf("NVT: СБРОС 92 ОТПУЩЕН, ПРЕРЫВАНИЕ 1 = %d\n",
            gpio_read(NVT_GPIO_IRQ));

    /* Инженерный сброс, как в драйвере перед первым опросом */
    nvt_write_addr(ENG_RST_ADDR, 0x5A);
    mdelay(1);

    for (int retry = 0; retry < 5; retry++) {
        const char *name;
        int r;

        nvt_bootloader_reset();

        if (nvt_set_page(CHIP_VER_TRIM_ADDR)) {
            kprintf("NVT: СТРАНИЦА НЕ ЗАДАНА\n");
            continue;
        }

        for (u32 i = 0; i < sizeof(buf); i++)
            buf[i] = 0;
        buf[0] = CHIP_VER_TRIM_ADDR & 0x7F;     /* 0x4E */

        r = nvt_read(buf, 7);
        kprintf("NVT: ПОПЫТКА %d -> %d, ИД %02x %02x %02x %02x %02x %02x\n",
                retry, r, buf[1], buf[2], buf[3], buf[4], buf[5], buf[6]);

        name = nvt_match(buf + 1);
        if (name) {
            kprintf("NVT: ЭТО %s\n", name);
            return;
        }
        mdelay(10);
    }
    kprintf("NVT: МИКРОСХЕМА НЕ ОПОЗНАНА\n");
}

#else
void nvt_probe(void) { }
int  nvt_set_page(u32 a) { (void)a; return -1; }
int  nvt_write_addr(u32 a, u8 d) { (void)a; (void)d; return -1; }
int  nvt_read(u8 *b, u32 n) { (void)b; (void)n; return -1; }
int  nvt_write(const u8 *b, u32 n) { (void)b; (void)n; return -1; }
#endif
