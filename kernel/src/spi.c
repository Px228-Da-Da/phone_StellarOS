/*
 * Контроллер SPI у MediaTek: пока только осмотр.
 *
 * Смещения из spi-mt65xx.c. Проверяем их до всякой записи: если блок не
 * тактируется, все регистры читаются нулями, и писать в них бессмысленно —
 * ровно так было с физическим уровнем USB.
 */
#include "spi.h"
#include "io.h"
#include "print.h"
#include "gpio.h"

#if defined(BOARD_MERLIN)
#include "soc/mt6768.h"

#define SPI_CFG0        0x00    /* длительности такта и выбора кристалла */
#define SPI_CFG1        0x04    /* длина посылки, паузы                  */
#define SPI_TX_SRC      0x08    /* адрес для передачи через DMA          */
#define SPI_RX_DST      0x0C
#define SPI_TX_DATA     0x10    /* очередь передачи без DMA              */
#define SPI_RX_DATA     0x14    /* очередь приёма                        */
#define SPI_CMD         0x18    /* запуск и режимы                       */
#define SPI_STATUS0     0x1C
#define SPI_PAD_SEL     0x24    /* какие выводы обслуживает контроллер   */
#define SPI_CFG2        0x28

/*
 * Что творится в контроллере тактирования.
 *
 * SPI0 читается нулями, значит блок не затактирован. Включает его
 * infracfg_ao@10001000 — у MediaTek там регистры «включить / выключить /
 * состояние», по биту на блок.
 *
 * Только чтение. Ошибка при записи сюда стоит дорого: рядом лежит
 * тактирование памяти и самого процессора, и снять его — значит
 * мгновенно остановить машину.
 */
void clk_probe(void)
{
    u32 shown = 0;

    kprintf("INFRACFG_AO 0x10001000, ЖИВЫЕ РЕГИСТРЫ:\n");
    for (u32 o = 0; o < 0x200; o += 4) {
        u32 v = mmio_read32(0x10001000UL + o);

        if (!v)
            continue;
        kprintf(" %03x=%08x", o, v);
        if (++shown % 4 == 0)
            kprintf("\n");
    }
    if (shown % 4)
        kprintf("\n");
    kprintf("INFRACFG: ВСЕГО %u\n", shown);
}

/* --- Тактирование ---
 * Адреса и биты взяты из открытых исходников ядра Xiaomi для merlin
 * (drivers/clk/mediatek/clk-mt6768.c), а не угаданы. Гадать здесь нельзя:
 * рядом лежит тактирование памяти и процессора, и ошибка останавливает
 * машину мгновенно, без паники и без вывода.
 *
 *   topckgen 0x10000000: CLK_CFG_2 = 0x60, SET 0x64, CLR 0x68
 *     поле spi_sel — сдвиг 24, ширина 2
 *     источники: 0 = clk26m, 1..3 = делители системной PLL
 *   infracfg_ao 0x10001000: ifr3 set 0x88, clr 0x8c, sta 0x94
 *     SPI0 — бит 1
 */
#define TOPCKGEN_BASE       0x10000000UL
#define CLK_CFG_2           0x60
#define CLK_CFG_2_SET       0x64
#define CLK_CFG_2_CLR       0x68
#define SPI_SEL_SHIFT       24
#define SPI_SEL_MASK        (0x3U << SPI_SEL_SHIFT)
#define SPI_SEL_PDN         (1U << 31)  /* запрет самого мультиплексора */

#define INFRACFG_BASE       0x10001000UL
#define IFR3_SET            0x88
#define IFR3_CLR            0x8C
#define IFR3_STA            0x94
#define IFR3_SPI0           (1U << 1)

/*
 * Включить тактирование SPI0.
 *
 * Два звена, и оба обязательны: выбрать источник частоты и снять запрет
 * на выдачу такта модулю. Загрузчик не делает ни того, ни другого —
 * шина ему не нужна, поэтому блок и читался нулями.
 *
 * Пишем только в регистры «снять»: они сбрасывают указанные биты и не
 * трогают соседние поля, где живёт тактирование всего остального.
 */
void spi_clk_enable(void)
{
    u32 cfg_before = mmio_read32(TOPCKGEN_BASE + CLK_CFG_2);
    u32 sta_before = mmio_read32(INFRACFG_BASE + IFR3_STA);

    kprintf("CLK: CLK_CFG_2 %08x, IFR3_STA %08x\n", cfg_before, sta_before);
    kprintf("CLK: SPI0 %s\n",
            (sta_before & IFR3_SPI0) ? "ЗАПРЕЩЁН" : "РАЗРЕШЁН");

    /* Источник: сбрасываем оба бита поля -> вариант 0, clk26m.
     * Двадцать шесть мегагерц, всегда доступен, тачу нужно восемь. */
    mmio_write32(TOPCKGEN_BASE + CLK_CFG_2_CLR, SPI_SEL_MASK);
    /* И снимаем запрет с самого мультиплексора */
    mmio_write32(TOPCKGEN_BASE + CLK_CFG_2_CLR, SPI_SEL_PDN);
    dsb();

    /* Снимаем запрет на выдачу такта модулю */
    mmio_write32(INFRACFG_BASE + IFR3_CLR, IFR3_SPI0);
    dsb();

    kprintf("CLK: СТАЛО CLK_CFG_2 %08x, IFR3_STA %08x\n",
            mmio_read32(TOPCKGEN_BASE + CLK_CFG_2),
            mmio_read32(INFRACFG_BASE + IFR3_STA));
}

/*
 * Жив ли блок на самом деле.
 *
 * Нули в регистрах я сгоряча принял за отсутствие тактирования, но это
 * неверно: у ненастроенного контроллера CFG0, CMD и STATUS0 и обязаны
 * быть нулевыми — таково их состояние после сброса.
 *
 * Настоящая проверка одна: записать значение и прочитать обратно.
 * Сохранилось — блок тактируется и отвечает. Не сохранилось — мёртв.
 *
 * Пишем в CFG0 (длительности такта): это чистая настройка, она ничего
 * не запускает и ни на что за пределами контроллера не влияет.
 */
void spi_alive_test(void)
{
    u32 saved = mmio_read32(MT_SPI0_BASE + SPI_CFG0);
    u32 back;

    mmio_write32(MT_SPI0_BASE + SPI_CFG0, 0x00100010);
    dsb();
    back = mmio_read32(MT_SPI0_BASE + SPI_CFG0);
    mmio_write32(MT_SPI0_BASE + SPI_CFG0, saved);
    dsb();

    kprintf("SPI0: ЗАПИСАЛ 00100010, ПРОЧИТАЛ %08x -> %s\n", back,
            back == 0x00100010 ? "БЛОК ЖИВ" : "БЛОК НЕ ОТВЕЧАЕТ");
}

void spi_probe(void)
{
    u32 nz = 0;

    for (u32 o = 0; o < 0x100; o += 4)
        if (mmio_read32(MT_SPI0_BASE + o))
            nz++;

    kprintf("SPI0 @ 0x%08lx, НЕНУЛЕВЫХ %u ИЗ 64\n", (u64)MT_SPI0_BASE, nz);
    kprintf("  CFG0 %08x CFG1 %08x CFG2 %08x\n",
            mmio_read32(MT_SPI0_BASE + SPI_CFG0),
            mmio_read32(MT_SPI0_BASE + SPI_CFG1),
            mmio_read32(MT_SPI0_BASE + SPI_CFG2));
    kprintf("  CMD  %08x STATUS0 %08x PAD %08x\n",
            mmio_read32(MT_SPI0_BASE + SPI_CMD),
            mmio_read32(MT_SPI0_BASE + SPI_STATUS0),
            mmio_read32(MT_SPI0_BASE + SPI_PAD_SEL));
}

/* --- Обмен по шине ---------------------------------------------
 *
 * Контроллер работает пакетами: в очередь кладутся байты, в CFG1
 * указывается длина, командой ACT запускается передача. Приём идёт
 * одновременно с передачей — у SPI это одна и та же операция, просто
 * по двум проводам.
 *
 * Очередь у контроллера 32 байта, поэтому длинные посылки требуют DMA.
 * Нам пока хватает коротких: команды тачскрину укладываются.
 */
#define SPI_CMD_ACT         (1U << 0)
#define SPI_CMD_RST         (1U << 2)
#define SPI_CMD_PAUSE_EN    (1U << 4)
#define SPI_CMD_DEASSERT    (1U << 5)
#define SPI_CMD_CPHA        (1U << 8)
#define SPI_CMD_CPOL        (1U << 9)
#define SPI_CMD_TXMSBF      (1U << 12)  /* старший бит первым */
#define SPI_CMD_RXMSBF      (1U << 13)
#define SPI_CMD_RX_ENDIAN   (1U << 14)
#define SPI_CMD_TX_ENDIAN   (1U << 15)

/* Наружу для отладки: без документации важно видеть не только «получилось
 * или нет», но и что именно показал контроллер и сколько мы ждали. */
u32 spi_last_status, spi_last_cmd, spi_last_spins;

/*
 * Отдать выводы контроллеру SPI.
 *
 * Номера и код функции взяты из исходников ядра Xiaomi
 * (pinctrl-mtk-mt6768.h), а не угаданы:
 *   GPIO32 -> SPI0_MI, GPIO33 -> SPI0_CSB,
 *   GPIO34 -> SPI0_MO, GPIO35 -> SPI0_CLK, функция 1
 *
 * Без этого шина работает вхолостую: контроллер честно отрабатывает
 * посылку, но сигналы до микросхемы не доходят, и в ответ приходят
 * сплошные единицы — линия просто висит подтянутой к питанию. Именно
 * это мы и наблюдали.
 */
void spi_pins_setup(void)
{
    gpio_set_mode(32, 1);               /* приём            */
    gpio_set_mode(33, 1);               /* выбор кристалла  */
    gpio_set_mode(34, 1);               /* передача         */
    gpio_set_mode(35, 1);               /* такт             */

    kprintf("SPI0: ВЫВОДЫ 32..35 -> РЕЖИМ %d %d %d %d\n",
            gpio_get_mode(32), gpio_get_mode(33),
            gpio_get_mode(34), gpio_get_mode(35));
}

/*
 * Тактовые задержки контроллера.
 *
 * У mt6768 узел spi0 объявлен как "mediatek,mt6765-spi", а у этой
 * разновидности в драйвере включён enhance_timing — и раскладка регистров
 * меняется целиком:
 *
 *   обычная:   CFG0 = такт_высокий | такт_низкий<<8 | удержание<<16 | установка<<24
 *   enhance:   CFG2 = (такт_высокий-1) | (такт_низкий-1)<<16
 *              CFG0 = (удержание-1)    | (установка-1)<<16
 *
 * Перепутать их не безобидно: в первый раз мы писали длительности в CFG0,
 * а CFG2 оставляли нулём — и контроллер понимал это как «полтакта = один
 * цикл», то есть гнал шину на 13 МГц вместо восьми разрешённых. Ответ
 * тачскрина при этом приходил, но осмысленным не был.
 */
#define SPI_CFG1_CS_IDLE_SHIFT      0
#define SPI_CFG1_PACKET_LOOP_SHIFT  8
#define SPI_CFG1_PACKET_LEN_SHIFT   16

/* Источник после spi_clk_enable() — clk26m */
#define SPI_PARENT_HZ       26000000U

/* Установка выбора кристалла: 25 тактов, как в настройках Novatek
 * из драйвера (struct mtk_chip_config spi_ctrdata). */
#define SPI_CS_SETUP        25U

static void spi_timing(u32 hz)
{
    u32 div = (hz && hz < SPI_PARENT_HZ / 2)
              ? (SPI_PARENT_HZ + hz - 1) / hz : 1;
    u32 sck = (div + 1) / 2;            /* половина периода, в тактах */
    u32 cs  = sck * 2;
    u32 cfg1;

    mmio_write32(MT_SPI0_BASE + SPI_CFG2,
                 ((sck - 1) & 0xFFFF) | (((sck - 1) & 0xFFFF) << 16));
    mmio_write32(MT_SPI0_BASE + SPI_CFG0,
                 ((cs - 1) & 0xFFFF) | (((SPI_CS_SETUP - 1) & 0xFFFF) << 16));

    cfg1 = mmio_read32(MT_SPI0_BASE + SPI_CFG1) & ~0xFFU;
    cfg1 |= (cs - 1) & 0xFF;            /* пауза между посылками */
    mmio_write32(MT_SPI0_BASE + SPI_CFG1, cfg1);
    dsb();
}

int spi_transfer(const u8 *tx, u8 *rx, u32 len)
{
    u32 cmd, spins = 0;

    if (!len || len > SPI_FIFO_MAX)
        return -1;

    /* Режим 0, старший бит первым, порядок байт как у процессора,
     * без DMA, без пауз, выбор кристалла снимается сам. */
    cmd = SPI_CMD_TXMSBF | SPI_CMD_RXMSBF;
    mmio_write32(MT_SPI0_BASE + SPI_CMD, cmd);
    mmio_write32(MT_SPI0_BASE + SPI_PAD_SEL, 0);
    dsb();

    /* Сброс до заполнения очереди, а не после.
     * Раньше было наоборот, и сброс выметал только что положенные байты. */
    mmio_write32(MT_SPI0_BASE + SPI_CMD, cmd | SPI_CMD_RST);
    dsb();
    mmio_write32(MT_SPI0_BASE + SPI_CMD, cmd);
    dsb();

    spi_timing(NVT_SPI_HZ);

    /* Длина пакета минус один; повтор один, значит поле повтора нулевое */
    {
        u32 cfg1 = mmio_read32(MT_SPI0_BASE + SPI_CFG1);

        cfg1 &= ~((0x3FFU << SPI_CFG1_PACKET_LEN_SHIFT) |
                  (0xFFU << SPI_CFG1_PACKET_LOOP_SHIFT));
        cfg1 |= (len - 1) << SPI_CFG1_PACKET_LEN_SHIFT;
        mmio_write32(MT_SPI0_BASE + SPI_CFG1, cfg1);
        dsb();
    }

    /* Кладём в очередь то, что отдаём. Даже при чтении посылать что-то
     * обязательно: такт на шине задаёт ведущий, и без передачи не будет
     * и приёма. */
    for (u32 i = 0; i < len; i += 4) {
        u32 w = 0;

        for (u32 b = 0; b < 4 && i + b < len; b++)
            w |= (u32)tx[i + b] << (8 * b);
        mmio_write32(MT_SPI0_BASE + SPI_TX_DATA, w);
    }
    dsb();

    mmio_write32(MT_SPI0_BASE + SPI_CMD, cmd | SPI_CMD_ACT);
    dsb();

    /* Ждём завершения. Ограничение обязательно: если раскладка регистров
     * неверна, ядро не должно повиснуть здесь навсегда. */
    while (!(mmio_read32(MT_SPI0_BASE + SPI_STATUS0) & 1) && ++spins < 200000)
        ;

    spi_last_status = mmio_read32(MT_SPI0_BASE + SPI_STATUS0);
    spi_last_cmd    = mmio_read32(MT_SPI0_BASE + SPI_CMD);
    spi_last_spins  = spins;

    if (spins >= 200000)
        return -2;

    if (rx) {
        for (u32 i = 0; i < len; i += 4) {
            u32 w = mmio_read32(MT_SPI0_BASE + SPI_RX_DATA);

            for (u32 b = 0; b < 4 && i + b < len; b++)
                rx[i + b] = (u8)(w >> (8 * b));
        }
    }
    return 0;
}

#else
void spi_probe(void) { }
void clk_probe(void) { }
void spi_clk_enable(void) { }
void spi_alive_test(void) { }
int  spi_transfer(const u8 *t, u8 *r, u32 n) { (void)t; (void)r; (void)n; return -1; }
void spi_pins_setup(void) { }
#endif
