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

#else
void spi_probe(void) { }
void clk_probe(void) { }
#endif
