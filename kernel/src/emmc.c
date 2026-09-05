/*
 * eMMC: первый заход — только смотрим.
 *
 * Ставка та же, что уже дважды сыграла в этом проекте с экраном и с
 * USB: загрузчик читал с eMMC наше собственное ядро, значит контроллер
 * он оставил настроенным и работающим. Если это так, нам не придётся
 * поднимать питание, тактирование и режим передачи с нуля — достаточно
 * повторить чтение на живом контроллере.
 *
 * Поэтому здесь нет ни одной записи в регистры. Только чтение и печать.
 * Причина не в осторожности вообще, а в конкретном свойстве этого
 * устройства: eMMC — единственное место в телефоне, где наша ошибка
 * необратима. Экран можно перерисовать, тачскрин перепрошить, а стёртый
 * раздел не вернуть ничем.
 *
 * Что мы хотим узнать этим заходом:
 *   1. отвечает ли контроллер вообще (не нули и не сплошные единицы);
 *   2. какие поля настроены — по ним видно, в каком режиме он оставлен;
 *   3. совпадает ли расположение регистров с тем, что мы о нём думаем.
 *
 * Третий пункт важнее первых двух. Регистровую карту MSDC мы знаем по
 * вендорному драйверу, но помнить — не то же самое, что знать: дамп
 * покажет, где на самом деле лежат ненулевые поля, и дальше мы будем
 * опираться на увиденное, а не на память.
 */
#include "types.h"
#include "io.h"
#include "print.h"

#if defined(BOARD_MERLIN)
#include "soc/mt6768.h"

/*
 * Адреса взяты из дерева устройств живого телефона
 * (device-info/dt-tree.txt):
 *
 *   msdc@11230000      — MSDC0, помечен non-removable: это eMMC
 *   msdc@11240000      — MSDC1, съёмная карта
 *   msdc0_top@11cd0000 — блок подстройки задержек для MSDC0
 *
 * Берём только нулевой: карты в телефоне нет, а раздел boot, из
 * которого нас и загрузили, лежит на eMMC.
 */
#define MSDC0_BASE      0x11230000UL
#define MSDC0_TOP_BASE  0x11cd0000UL

#define DUMP_WORDS      64      /* первые 256 байт: там всё главное */

static void dump_block(const char *what, u64 base, u32 words)
{
    for (u32 i = 0; i < words; i += 4) {
        kprintf("%s +%02x: %08x %08x %08x %08x\n", what, i * 4,
                mmio_read32(base + (i + 0) * 4),
                mmio_read32(base + (i + 1) * 4),
                mmio_read32(base + (i + 2) * 4),
                mmio_read32(base + (i + 3) * 4));
    }
}

/*
 * Признак того, что перед нами живой блок, а не пустое место.
 *
 * Отключённый блок на MediaTek читается либо нулями, либо сплошными
 * единицами — и то и другое означает «шина ответила за него сама».
 * Смешанные значения означают, что регистры настоящие.
 */
static int looks_alive(u64 base, u32 words)
{
    u32 zeros = 0, ones = 0;

    for (u32 i = 0; i < words; i++) {
        u32 v = mmio_read32(base + i * 4);

        if (v == 0)
            zeros++;
        else if (v == 0xFFFFFFFF)
            ones++;
    }

    return zeros != words && ones != words;
}


/* ---------------------------------------------------------------------
 * Чтение блока.
 *
 * Всё, что здесь есть, опирается на увиденное в дампе живого
 * контроллера, а не на память о вендорном драйвере. Дамп показал:
 *
 *   MSDC_CFG  02200199   бит 3 взведён — передача идёт через FIFO,
 *                        без DMA. Значит и нам не нужен DMA.
 *   SDC_CMD   0200008d   индекс команды в младших битах (0x0d = 13),
 *                        тип ответа в битах 7..9 (1 = R1),
 *                        длина блока в битах 16..27 (0x200 = 512).
 *   SDC_ARG   00010000   адрес карты (RCA) = 1 в старшей половине.
 *   RESP0     00000900   состояние 4 «передача», готова к данным.
 *
 * Отсюда команда чтения одного блока собирается однозначно: индекс 17,
 * ответ R1, признак «есть данные», направление — чтение, длина 512.
 *
 * Про безопасность. Записи здесь есть, но только в регистры
 * контроллера — те, которыми задаётся команда. Самой карте мы посылаем
 * команду ЧТЕНИЯ; изменить ею содержимое разделов невозможно в
 * принципе. Ни одной команды записи, стирания или переключения разделов
 * в этом файле нет и не будет, пока чтение не станет надёжным.
 */
#define MSDC_CFG            0x00
#define MSDC_IOCON          0x04
#define MSDC_PS             0x08
#define MSDC_INT            0x0c
#define MSDC_INTEN          0x10
#define MSDC_FIFOCS         0x14
#define MSDC_RXDATA         0x1c
#define SDC_CFG             0x30
#define SDC_CMD             0x34
#define SDC_ARG             0x38
#define SDC_STS             0x3c
#define SDC_RESP0           0x40
#define SDC_BLK_NUM         0x50

#define MSDC_CFG_PIO        (1U << 3)

#define MSDC_INT_CMDRDY     (1U << 8)
#define MSDC_INT_CMDTMO     (1U << 9)
#define MSDC_INT_RSPCRCERR  (1U << 10)
#define MSDC_INT_XFER_COMPL (1U << 12)
#define MSDC_INT_DATTMO     (1U << 14)
#define MSDC_INT_DATCRCERR  (1U << 15)

#define SDC_STS_SDCBUSY     (1U << 0)
#define SDC_STS_CMDBUSY     (1U << 1)

#define FIFOCS_RXCNT(v)     ((v) & 0xFF)
#define FIFOCS_CLR          (1U << 31)

/* Собрать слово команды: длина блока, тип ответа и индекс */
#define CMD_WORD(op, resp, dtype, rw, blklen) \
    (((blklen) << 16) | ((rw) << 13) | ((dtype) << 11) | ((resp) << 7) | (op))

#define CMD_READ_SINGLE     CMD_WORD(17, 1, 1, 0, 512)

/* Ждать обнуления битов маски. 0 — не дождались. */
static int wait_until_zero(u64 reg, u32 mask, u32 ms)
{
    u64 deadline = read_cntpct() + read_cntfrq() * ms / 1000;

    while (mmio_read32(reg) & mask)
        if (read_cntpct() > deadline)
            return 0;
    return 1;
}

/* Ждать, пока условие станет истинным. 0 — не дождались. */
static int wait_until(u64 reg, u32 mask, int set, u32 ms)
{
    u64 deadline = read_cntpct() + read_cntfrq() * ms / 1000;

    for (;;) {
        u32 v = mmio_read32(reg);

        if (set ? (v & mask) : !(v & mask))
            return 1;
        if (read_cntpct() > deadline)
            return 0;
    }
}

/*
 * Прочитать один блок по номеру сектора.
 *
 * Возвращает 0 при успехе. Ошибки различаем по причинам: карта не
 * ответила на команду, ответ пришёл с ошибкой контрольной суммы, данные
 * не пришли вовсе. Разница важна: первое означает, что мы неправильно
 * собрали команду, последнее — что неправильно читаем данные.
 */
int emmc_read_block(u64 lba, void *dst)
{
    u32 *out = dst;
    u32 got = 0;
    u32 cfg = mmio_read32(MSDC0_BASE + MSDC_CFG);

    if (!(cfg & MSDC_CFG_PIO)) {
        kprintf("EMMC     : КОНТРОЛЛЕР НЕ В РЕЖИМЕ FIFO, ЧИТАТЬ НЕ БУДУ\n");
        return -1;
    }

    if (!wait_until(MSDC0_BASE + SDC_STS, SDC_STS_SDCBUSY | SDC_STS_CMDBUSY,
                    0, 100)) {
        kprintf("EMMC     : КОНТРОЛЛЕР ЗАНЯТ, СОСТОЯНИЕ %08x\n",
                mmio_read32(MSDC0_BASE + SDC_STS));
        return -1;
    }

    /*
     * Очищаем очередь данных ПЕРЕД командой.
     *
     * В первом же дампе, снятом до всякого чтения, в MSDC_FIFOCS стояло
     * 0000007c — сто двадцать четыре байта, оставшихся от загрузчика.
     * Без очистки мы вычерпываем их и принимаем за свои: в отчёте
     * появляются пятьсот двенадцать байт нулей и «передача не закрыта»,
     * потому что настоящая передача при этом даже не начиналась.
     */
    mmio_write32(MSDC0_BASE + MSDC_FIFOCS, FIFOCS_CLR);
    dsb();
    if (!wait_until_zero(MSDC0_BASE + MSDC_FIFOCS, 0x00FF00FF, 50))
        kprintf("EMMC     : ОЧЕРЕДЬ НЕ ОЧИСТИЛАСЬ, FIFOCS %08x\n",
                mmio_read32(MSDC0_BASE + MSDC_FIFOCS));

    /* Сбрасываем накопленные признаки: иначе примем чужой за свой */
    mmio_write32(MSDC0_BASE + MSDC_INT, 0xFFFFFFFF);
    mmio_write32(MSDC0_BASE + SDC_BLK_NUM, 1);
    mmio_write32(MSDC0_BASE + SDC_ARG, (u32)lba);
    dsb();
    mmio_write32(MSDC0_BASE + SDC_CMD, CMD_READ_SINGLE);
    dsb();

    if (!wait_until(MSDC0_BASE + MSDC_INT, MSDC_INT_CMDRDY, 1, 200)) {
        kprintf("EMMC     : КАРТА НЕ ОТВЕТИЛА НА КОМАНДУ, INT %08x\n",
                mmio_read32(MSDC0_BASE + MSDC_INT));
        return -1;
    }

    {
        u32 st = mmio_read32(MSDC0_BASE + MSDC_INT);

        if (st & (MSDC_INT_CMDTMO | MSDC_INT_RSPCRCERR)) {
            kprintf("EMMC     : ОТВЕТ С ОШИБКОЙ, INT %08x\n", st);
            return -1;
        }

        /*
         * Что именно ответила карта. R1 — это её состояние: биты 12..9
         * говорят, в каком она режиме, бит 8 — готова ли к данным.
         * Если здесь осмысленный ответ, значит команда собрана верно, и
         * искать расхождение надо дальше, в приёме данных.
         */
        kprintf("EMMC     : ОТВЕТ %08x, INT %08x, СОСТ %08x, ОЧЕРЕДЬ %08x\n",
                mmio_read32(MSDC0_BASE + SDC_RESP0), st,
                mmio_read32(MSDC0_BASE + SDC_STS),
                mmio_read32(MSDC0_BASE + MSDC_FIFOCS));
    }

    /*
     * Забираем данные из FIFO по мере появления. Ждём с ограничением:
     * если карта замолчит, зависнуть здесь означало бы потерять
     * загрузку, а мы всего лишь читали сектор.
     */
    {
        u64 deadline = read_cntpct() + read_cntfrq() / 2;   /* полсекунды */

        while (got < 512 / 4) {
            u32 have = FIFOCS_RXCNT(mmio_read32(MSDC0_BASE + MSDC_FIFOCS));

            while (have >= 4 && got < 512 / 4) {
                out[got++] = mmio_read32(MSDC0_BASE + MSDC_RXDATA);
                have -= 4;
            }

            if (read_cntpct() > deadline) {
                kprintf("EMMC     : ДАННЫЕ НЕ ПРИШЛИ, СЛОВ %u, INT %08x\n",
                        got, mmio_read32(MSDC0_BASE + MSDC_INT));
                return -1;
            }
        }
    }

    kprintf("EMMC     : ПРИНЯТО СЛОВ %u, INT %08x, ОЧЕРЕДЬ %08x\n",
            got, mmio_read32(MSDC0_BASE + MSDC_INT),
            mmio_read32(MSDC0_BASE + MSDC_FIFOCS));

    if (!wait_until(MSDC0_BASE + MSDC_INT, MSDC_INT_XFER_COMPL, 1, 200))
        kprintf("EMMC     : ДАННЫЕ ЕСТЬ, НО ПЕРЕДАЧА НЕ ЗАКРЫТА, INT %08x\n",
                mmio_read32(MSDC0_BASE + MSDC_INT));

    return 0;
}

/*
 * Прочитать нулевой и первый секторы и сказать, что в них.
 *
 * Проверка здесь не «получилось ли», а «то ли получилось»: в первом
 * секторе диска с GPT лежит защитная запись MBR, а во втором —
 * заголовок с сигнатурой EFI PART. Если она появится, значит мы читаем
 * настоящие данные с настоящего места, а не мусор из FIFO.
 */
static u8 sector[512] __attribute__((aligned(8)));

void emmc_read_probe(void)
{
    static const char sig[8] = { 'E', 'F', 'I', ' ', 'P', 'A', 'R', 'T' };

    if (emmc_read_block(0, sector) != 0) {
        kprintf("EMMC     : НУЛЕВОЙ СЕКТОР НЕ ПРОЧИТАН\n");
        return;
    }

    kprintf("EMMC     : СЕКТОР 0: %02x %02x %02x %02x, ПОДПИСЬ %02x%02x\n",
            sector[0], sector[1], sector[2], sector[3],
            sector[510], sector[511]);

    if (emmc_read_block(1, sector) != 0) {
        kprintf("EMMC     : ПЕРВЫЙ СЕКТОР НЕ ПРОЧИТАН\n");
        return;
    }

    for (u32 i = 0; i < 8; i++) {
        if (sector[i] != (u8)sig[i]) {
            kprintf("EMMC     : В СЕКТОРЕ 1 НЕ EFI PART: %02x %02x %02x %02x\n",
                    sector[0], sector[1], sector[2], sector[3]);
            return;
        }
    }

    kprintf("EMMC     : СЕКТОР 1 — EFI PART. ЧТЕНИЕ РАБОТАЕТ.\n");
}

void emmc_probe(void)
{
    kprintf("EMMC     : СМОТРЮ MSDC0 %p (ТОЛЬКО ЧТЕНИЕ)\n",
            (void *)MSDC0_BASE);

    if (!looks_alive(MSDC0_BASE, DUMP_WORDS)) {
        kprintf("EMMC     : КОНТРОЛЛЕР МОЛЧИТ — ВСЕ РЕГИСТРЫ ОДИНАКОВЫ\n");
        return;
    }

    dump_block("EMMC MSDC0", MSDC0_BASE, DUMP_WORDS);
    dump_block("EMMC TOP  ", MSDC0_TOP_BASE, 16);

    kprintf("EMMC     : КОНТРОЛЛЕР ОТВЕЧАЕТ, ПРОБУЮ ПРОЧИТАТЬ\n");
    emmc_read_probe();
}

#else   /* в эмуляторе eMMC нет: там ядро приходит из командной строки */

void emmc_probe(void)
{
    kprintf("EMMC     : В ЭМУЛЯТОРЕ КОНТРОЛЛЕРА НЕТ\n");
}

int emmc_read_block(u64 lba, void *dst)
{
    (void)lba; (void)dst;
    return -1;
}

#endif
