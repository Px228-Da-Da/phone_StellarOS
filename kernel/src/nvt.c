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

/* Снимки состояния шины: без них непонятно, почему посылка не прошла */
extern u32 spi_last_status, spi_last_cmd, spi_last_spins;

/* Момент защёлкивания принятого бита: подбираем на устройстве */
extern u32 spi_sample_sel, spi_tick_delay;
extern u32 spi_lost_status;
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

u8  nvt_last_rx[SPI_FIFO_MAX];
u32 nvt_last_rx_len;

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

    /* Сохраняем принятое как есть: при разборе протокола важно видеть
     * не итог, а сырой поток — по нему сразу видно, съехали данные или нет. */
    for (u32 i = 0; i < len + 1 && i < SPI_FIFO_MAX; i++)
        nvt_last_rx[i] = rx[i];
    nvt_last_rx_len = len + 1;

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
    /* Вендорный драйвер ждёт 10 мс, но у него микросхема к этому моменту
     * под питанием уже давно. У нас питание подано только что, поэтому
     * даём ей встать полностью: в первый раз, когда пауза была 200 мс,
     * идентификатор прочитался с первой попытки. */
    mdelay(200);

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


/* --- Загрузка прошивки -------------------------------------------
 *
 * У NT36672A нет своей флеш-памяти: прошивку заливает хост при каждом
 * включении, иначе контроллер молчит. Именно поэтому касания и не
 * приходили — микросхема отвечала на опрос, но работать ей было нечем.
 *
 * Карта памяти взята из nt36xxx_mem_map.h, ветка NT36672A_memory_map.
 * У этой микросхемы hw_crc = 1: контрольные суммы считает она сама,
 * нам достаточно сообщить ей адреса, длины и эталонные суммы из файла.
 */
#define EVENT_BUF_ADDR          0x21C00
#define BOOT_RDY_ADDR           0x3F10D
#define ILM_LENGTH_ADDR         0x3F118
#define DLM_LENGTH_ADDR         0x3F130
#define ILM_DES_ADDR            0x3F128
#define DLM_DES_ADDR            0x3F12C
#define G_ILM_CHECKSUM_ADDR     0x3F100
#define G_DLM_CHECKSUM_ADDR     0x3F104
#define R_ILM_CHECKSUM_ADDR     0x3F120
#define R_DLM_CHECKSUM_ADDR     0x3F124
#define BLD_CRC_EN_ADDR         0x3F30E
#define BLD_ILM_DLM_CRC_ADDR    0x3F133

/* Смещения внутри буфера событий */
#define EVENT_MAP_HOST_CMD          0x50
#define EVENT_MAP_RESET_COMPLETE    0x60
#define EVENT_MAP_FWINFO            0x78

#define RESET_STATE_INIT        0xA0
#define RESET_STATE_MAX         0xAF

/* Прошивка лежит в самом образе ядра: файловой системы у нас нет */
extern const u8  nvt_fw[];
extern const u32 nvt_fw_size;

static u32 le32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

/*
 * Разбор заголовка файла.
 *
 * Первые две записи по 12 байт — ILM и DLM, сама прошивка. За ними, с
 * 0x30, идут информационные секции по 16 байт. Сколько их — говорит
 * первое слово файла: это конец заголовка.
 */
struct nvt_part {
    u32 bin;        /* смещение в файле         */
    u32 sram;       /* куда класть в микросхеме */
    u32 size;
    u32 crc;
};

#define NVT_MAX_PARTS   40
static struct nvt_part nvt_parts[NVT_MAX_PARTS];
static u32 nvt_nparts;

static int nvt_parse_header(const u8 *fw, u32 fwsize)
{
    u32 end = le32(fw);
    u32 info = 0, ovly = 0, list;
    u8  ovly_info = (u8)((fw[0x28] & 0x10) >> 4);

    for (u32 pos = 0x30; pos < end; pos += 0x10)
        info++;
    if (ovly_info)
        ovly = fw[0x28] & 0x0F;

    nvt_nparts = 2 + ovly + info;
    if (nvt_nparts > NVT_MAX_PARTS) {
        kprintf("NVT: СЕКЦИЙ %u, БОЛЬШЕ ЧЕМ ВЛЕЗАЕТ\n", nvt_nparts);
        return -1;
    }

    for (list = 0; list < nvt_nparts; list++) {
        u32 pos;

        if (list < 2) {
            /* ILM и DLM: адрес в файле, адрес в памяти, длина.
             * Контрольная сумма лежит отдельно, на 0x18 и 0x1C. */
            nvt_parts[list].bin  = le32(&fw[0 + list * 12]);
            nvt_parts[list].sram = le32(&fw[4 + list * 12]);
            nvt_parts[list].size = le32(&fw[8 + list * 12]);
            nvt_parts[list].crc  = le32(&fw[0x18 + list * 4]);
        } else if (list < 2 + info) {
            pos = 0x30 + 0x10 * (list - 2);
            nvt_parts[list].sram = le32(&fw[pos]);
            nvt_parts[list].size = le32(&fw[pos + 4]);
            nvt_parts[list].bin  = le32(&fw[pos + 8]);
            nvt_parts[list].crc  = le32(&fw[pos + 12]);
        } else {
            /* Секции перекрытия описаны не в заголовке, а в начале DLM */
            pos = nvt_parts[1].bin + 0x10 * (list - 2 - info);
            nvt_parts[list].sram = le32(&fw[pos]);
            nvt_parts[list].size = le32(&fw[pos + 4]);
            nvt_parts[list].bin  = le32(&fw[pos + 8]);
            nvt_parts[list].crc  = le32(&fw[pos + 12]);
        }

        if (nvt_parts[list].bin + nvt_parts[list].size > fwsize) {
            kprintf("NVT: СЕКЦИЯ %u ВЫХОДИТ ЗА ФАЙЛ\n", list);
            return -1;
        }
    }
    return 0;
}

/*
 * Заливка одной секции.
 *
 * Драйвер шлёт по 63 КБ за раз через DMA, у нас же вся передача идёт
 * через очередь контроллера на 32 байта: адрес плюс 31 байт данных.
 * Микросхеме это безразлично — счётчик адреса у неё внутренний, и
 * каждый кусок мы адресуем заново.
 */
#define NVT_CHUNK   (SPI_FIFO_MAX - 1)

static int nvt_write_sram(const u8 *fw, u32 sram, u32 size, u32 bin)
{
    u8 buf[SPI_FIFO_MAX];

    while (size) {
        u32 room = 0x80 - (sram & 0x7F);    /* до конца страницы */
        u32 len = size > NVT_CHUNK ? NVT_CHUNK : size;

        if (len > room)
            len = room;
        if (nvt_set_page(sram))
            return -1;
        buf[0] = sram & 0x7F;
        for (u32 i = 0; i < len; i++)
            buf[1 + i] = fw[bin + i];
        if (nvt_write(buf, len + 1))
            return -1;

        sram += len;
        bin  += len;
        size -= len;
    }
    return 0;
}

static int nvt_write_firmware(const u8 *fw)
{
    for (u32 list = 0; list < nvt_nparts; list++) {
        u32 size = nvt_parts[list].size;

        if (!size)                      /* пустые секции пропускаем */
            continue;
        size++;                         /* так делает вендорный драйвер */
        if (nvt_write_sram(fw, nvt_parts[list].sram, size, nvt_parts[list].bin)) {
            kprintf("NVT: СЕКЦИЯ %u НЕ ЗАЛИЛАСЬ, SPI СОСТ %08x КМД %08x ОЖИД %u\n",
                    list, spi_last_status, spi_last_cmd, spi_last_spins);
            return -1;
        }
    }
    return 0;
}

/*
 * Сообщить микросхеме, что и где проверять.
 *
 * Три поля лежат в одной 128-байтовой странице, поэтому страницу
 * задаём один раз — так же поступает вендорный драйвер.
 */
static void nvt_set_crc_bank(u32 des_addr, u32 sram, u32 len_addr, u32 size,
                             u32 sum_addr, u32 crc)
{
    u8 buf[5];

    nvt_set_page(des_addr);

    buf[0] = des_addr & 0x7F;
    buf[1] = (u8)(sram);
    buf[2] = (u8)(sram >> 8);
    buf[3] = (u8)(sram >> 16);
    nvt_write(buf, 4);

    buf[0] = len_addr & 0x7F;
    buf[1] = (u8)(size);
    buf[2] = (u8)(size >> 8);
    nvt_write(buf, 3);                  /* при hw_crc = 1 длина двухбайтовая */

    buf[0] = sum_addr & 0x7F;
    buf[1] = (u8)(crc);
    buf[2] = (u8)(crc >> 8);
    buf[3] = (u8)(crc >> 16);
    buf[4] = (u8)(crc >> 24);
    nvt_write(buf, 5);
}

static void nvt_bld_crc_enable(void)
{
    u8 buf[2];

    nvt_set_page(BLD_CRC_EN_ADDR);
    buf[0] = BLD_CRC_EN_ADDR & 0x7F;
    buf[1] = 0xFF;
    nvt_read(buf, 2);

    buf[0] = BLD_CRC_EN_ADDR & 0x7F;
    buf[1] = (u8)(buf[1] | (1 << 7));
    nvt_write(buf, 2);
}

static void nvt_fw_crc_enable(void)
{
    u8 buf[2];

    nvt_set_page(EVENT_BUF_ADDR);

    buf[0] = EVENT_MAP_RESET_COMPLETE;  /* сбрасываем признак сброса */
    buf[1] = 0x00;
    nvt_write(buf, 2);

    buf[0] = EVENT_MAP_HOST_CMD;        /* включаем проверку суммы прошивки */
    buf[1] = 0xAE;
    nvt_write(buf, 2);
}

static void nvt_boot_ready(void)
{
    nvt_write_addr(BOOT_RDY_ADDR, 1);
    mdelay(5);
}

/* Дождаться, пока прошивка отчитается о запуске */
static int nvt_check_reset_state(void)
{
    u8 buf[8];

    nvt_set_page(EVENT_BUF_ADDR | EVENT_MAP_RESET_COMPLETE);

    for (int retry = 0; retry <= 10; retry++) {
        buf[0] = EVENT_MAP_RESET_COMPLETE;
        buf[1] = 0x00;
        nvt_read(buf, 6);

        if (buf[1] >= RESET_STATE_INIT && buf[1] <= RESET_STATE_MAX)
            return 0;
        mdelay(10);
    }
    kprintf("NVT: ПРОШИВКА НЕ ЗАПУСТИЛАСЬ, СОСТОЯНИЕ %02x\n", buf[1]);
    return -1;
}

/* Что о себе сообщает поднявшаяся прошивка */
u16 nvt_abs_x_max = 1080, nvt_abs_y_max = 2340;

static void nvt_read_fw_info(void)
{
    u8 buf[20];

    nvt_set_page(EVENT_BUF_ADDR | EVENT_MAP_FWINFO);
    for (u32 i = 0; i < sizeof(buf); i++)
        buf[i] = 0;
    buf[0] = EVENT_MAP_FWINFO;
    nvt_read(buf, 17);

    /* Версия и её дополнение обязаны в сумме давать 0xFF: так проверяют,
     * что прочитали именно сведения, а не мусор. */
    if ((u8)(buf[1] + buf[2]) != 0xFF) {
        kprintf("NVT: СВЕДЕНИЯ БИТЫЕ, ВЕРСИЯ %02x ДОПОЛНЕНИЕ %02x\n",
                buf[1], buf[2]);
        return;
    }

    nvt_abs_x_max = (u16)((buf[5] << 8) | buf[6]);
    nvt_abs_y_max = (u16)((buf[7] << 8) | buf[8]);

    kprintf("NVT: ВЕРСИЯ %02x, СЕТКА %ux%u, ПОЛЕ %ux%u\n",
            buf[1], buf[3], buf[4], nvt_abs_x_max, nvt_abs_y_max);
}

/*
 * Почему прошивка не поднялась.
 *
 * Микросхема сама считает контрольные суммы залитого и сравнивает их с
 * эталонными, которые мы ей сообщили. Три вопроса, и на все есть ответ
 * в её регистрах: досчитала ли она, сошлись ли суммы и какие именно
 * значения она видит. Это отличает «данные приехали битыми» от «мы не
 * так сказали, что проверять».
 */
static u32 nvt_read32(u32 addr)
{
    u8 buf[6];

    nvt_set_page(addr);
    for (u32 i = 0; i < sizeof(buf); i++)
        buf[i] = 0;
    buf[0] = addr & 0x7F;
    if (nvt_read(buf, 5))
        return 0xFFFFFFFF;
    return (u32)buf[1] | ((u32)buf[2] << 8) |
           ((u32)buf[3] << 16) | ((u32)buf[4] << 24);
}

static void nvt_report_crc(void)
{
    u8 buf[2];

    nvt_set_page(BLD_ILM_DLM_CRC_ADDR);
    buf[0] = BLD_ILM_DLM_CRC_ADDR & 0x7F;
    buf[1] = 0x00;
    nvt_read(buf, 2);
    kprintf("NVT: СЧЁТ ЗАВЕРШЁН %u, ILM СОШЁЛСЯ %u, DLM СОШЁЛСЯ %u\n",
            (buf[1] >> 2) & 1, (buf[1] >> 0) & 1, (buf[1] >> 1) & 1);

    kprintf("NVT: ILM ФАЙЛ %08x ЭТАЛОН %08x СЧИТАНО %08x\n",
            nvt_parts[0].crc,
            nvt_read32(G_ILM_CHECKSUM_ADDR),
            nvt_read32(R_ILM_CHECKSUM_ADDR));
    kprintf("NVT: DLM ФАЙЛ %08x ЭТАЛОН %08x СЧИТАНО %08x\n",
            nvt_parts[1].crc,
            nvt_read32(G_DLM_CHECKSUM_ADDR),
            nvt_read32(R_DLM_CHECKSUM_ADDR));
}

/* Сверка залитого: читаем обратно кусок ILM и сравниваем с файлом.
 * Отвечает на самый простой вопрос — доехали ли байты вообще. */
static void nvt_verify_sram(const u8 *fw)
{
    u32 sram = nvt_parts[0].sram;
    u32 bin  = nvt_parts[0].bin;
    u8  buf[SPI_FIFO_MAX];
    u32 bad = 0;

    for (u32 blk = 0; blk < 8; blk++) {
        u32 off = (nvt_parts[0].size / 8) * blk;    /* пробы по всей секции */

        if (off + 16 > nvt_parts[0].size)
            break;
        nvt_set_page(sram + off);
        for (u32 i = 0; i < sizeof(buf); i++)
            buf[i] = 0;
        buf[0] = (sram + off) & 0x7F;
        if (nvt_read(buf, 17))
            continue;
        for (u32 i = 1; i < 17; i++)
            if (buf[i] != fw[bin + off + i - 1])
                bad++;
    }
    kprintf("NVT: СВЕРКА ILM, РАСХОЖДЕНИЙ %u ИЗ 128\n", bad);
}

int nvt_download(void)
{
    const u8 *fw = nvt_fw;
    u32 size = nvt_fw_size;

    kprintf("NVT: ПРОШИВКА В ОБРАЗЕ, %u БАЙТ\n", size);

    if (nvt_parse_header(fw, size))
        return -1;
    kprintf("NVT: СЕКЦИЙ %u, ILM %u БАЙТ, DLM %u БАЙТ\n",
            nvt_nparts, nvt_parts[0].size, nvt_parts[1].size);

    for (int retry = 0; retry < 3; retry++) {
        nvt_bootloader_reset();

        if (nvt_write_firmware(fw))
            continue;

        nvt_set_crc_bank(ILM_DES_ADDR, nvt_parts[0].sram, ILM_LENGTH_ADDR,
                         nvt_parts[0].size, G_ILM_CHECKSUM_ADDR,
                         nvt_parts[0].crc);
        nvt_set_crc_bank(DLM_DES_ADDR, nvt_parts[1].sram, DLM_LENGTH_ADDR,
                         nvt_parts[1].size, G_DLM_CHECKSUM_ADDR,
                         nvt_parts[1].crc);

        nvt_bld_crc_enable();
        nvt_fw_crc_enable();
        nvt_boot_ready();

        kprintf("NVT: ПОТЕРЯНО ПРИЗНАКОВ ЗАВЕРШЕНИЯ %u\n", spi_lost_status);
        nvt_report_crc();
        nvt_verify_sram(fw);

        if (nvt_check_reset_state() == 0) {
            kprintf("NVT: ПРОШИВКА ПОДНЯЛАСЬ С ПОПЫТКИ %d\n", retry + 1);
            nvt_read_fw_info();
            return 0;
        }
    }
    return -1;
}

/*
 * Прочитать сообщение о касаниях.
 *
 * Микросхема отдаёт 65 байт: по шесть на каждый из десяти пальцев плюс
 * хвост с давлением. За раз очередь контроллера тянет 31 байт, поэтому
 * читаем в несколько заходов — весь буфер лежит в одной странице, так
 * что смещение внутри неё можно задавать напрямую.
 */
#define NVT_POINT_LEN   65

static int nvt_read_points(u8 *out)
{
    u32 done = 0;

    nvt_set_page(EVENT_BUF_ADDR);

    while (done < NVT_POINT_LEN) {
        u8 buf[SPI_FIFO_MAX];
        u32 len = NVT_POINT_LEN - done;

        if (len > NVT_CHUNK)
            len = NVT_CHUNK;

        buf[0] = (u8)done;              /* смещение внутри буфера событий */
        if (nvt_read(buf, len + 1))
            return -1;
        for (u32 i = 0; i < len; i++)
            out[done + i] = buf[1 + i];
        done += len;
    }
    return 0;
}

/*
 * Разобрать сообщение в касания.
 *
 * Разбор списан с вендорного драйвера: шесть байт на палец, младшие три
 * бита первого байта — состояние (1 и 2 значат, что палец на стекле),
 * старшие пять — его номер. Координаты упакованы по двенадцать бит:
 * старшие восемь лежат в отдельных байтах, младшие четыре — в общем.
 */
int nvt_get_touches(struct nvt_touch *t, int max)
{
    u8 pd[NVT_POINT_LEN];
    int n = 0;

    if (nvt_read_points(pd))
        return -1;

    for (int i = 0; i < 10 && n < max; i++) {
        u32 pos = 6 * i;
        u8 id = (u8)(pd[pos] >> 3);
        u8 st = (u8)(pd[pos] & 0x07);
        u16 x, y;

        if (id == 0 || id > 10)
            continue;
        if (st != 1 && st != 2)
            continue;

        x = (u16)(((u32)pd[pos + 1] << 4) + (pd[pos + 3] >> 4));
        y = (u16)(((u32)pd[pos + 2] << 4) + (pd[pos + 3] & 0x0F));
        if (x > nvt_abs_x_max || y > nvt_abs_y_max)
            continue;

        t[n].id = id;
        t[n].x  = x;
        t[n].y  = y;
        t[n].w  = pd[pos + 4] ? pd[pos + 4] : 1;
        n++;
    }
    return n;
}

/* Одна попытка опознания при текущих настройках защёлкивания */
static const char *nvt_identify(void)
{
    u8 buf[8];

    nvt_bootloader_reset();
    if (nvt_set_page(CHIP_VER_TRIM_ADDR))
        return 0;

    for (u32 i = 0; i < sizeof(buf); i++)
        buf[i] = 0;
    buf[0] = CHIP_VER_TRIM_ADDR & 0x7F;     /* 0x4E */
    if (nvt_read(buf, 7))
        return 0;

    kprintf("NVT: ФРОНТ %u ЗАДЕРЖКА %u -> %02x %02x %02x %02x %02x %02x\n",
            spi_sample_sel, spi_tick_delay,
            buf[1], buf[2], buf[3], buf[4], buf[5], buf[6]);

    return nvt_match(buf + 1);
}

void nvt_probe(void)
{
    const char *name = 0;

    nvt_hw_reset();
    kprintf("NVT: СБРОС 92 ОТПУЩЕН, ПРЕРЫВАНИЕ 1 = %d\n",
            gpio_read(NVT_GPIO_IRQ));

    /* Инженерный сброс, как в драйвере перед первым опросом */
    nvt_write_addr(ENG_RST_ADDR, 0x5A);
    mdelay(1);

    /*
     * Подбираем момент защёлкивания принятого бита.
     *
     * Гадать тут нечего и незачем: вариантов всего восемь, а верный
     * виден сразу — идентификатор либо совпадает с таблицей, либо нет.
     * Одна попытка на устройстве стоит перепрошивки, поэтому перебираем
     * все за один заход, а не по одному варианту на прошивку.
     */
    for (u32 v = 0; v < 8 && !name; v++) {
        spi_sample_sel = v & 1;
        spi_tick_delay = v >> 1;
        name = nvt_identify();
    }

    if (!name) {
        kprintf("NVT: МИКРОСХЕМА НЕ ОПОЗНАНА НИ ПРИ ОДНОЙ НАСТРОЙКЕ\n");
        return;
    }

    kprintf("NVT: ЭТО %s, ФРОНТ %u ЗАДЕРЖКА %u\n",
            name, spi_sample_sel, spi_tick_delay);

    if (nvt_download())
        kprintf("NVT: ПРОШИВКУ ЗАЛИТЬ НЕ ВЫШЛО\n");
}

#else
void nvt_probe(void) { }
int  nvt_set_page(u32 a) { (void)a; return -1; }
int  nvt_write_addr(u32 a, u8 d) { (void)a; (void)d; return -1; }
int  nvt_read(u8 *b, u32 n) { (void)b; (void)n; return -1; }
int  nvt_write(const u8 *b, u32 n) { (void)b; (void)n; return -1; }
int  nvt_download(void) { return -1; }
int  nvt_get_touches(struct nvt_touch *t, int m) { (void)t; (void)m; return -1; }
u16  nvt_abs_x_max, nvt_abs_y_max;
#endif
