/*
 * Контроллер USB 2.0 (MUSB) у MediaTek: пока только осмотр.
 *
 * MUSB устроен непривычно: регистры конечных точек не разложены по адресам,
 * а видны через одно окно. Сначала пишется номер точки в регистр INDEX,
 * и только потом окно показывает её регистры. Поэтому читать их вразнобой
 * из разных мест кода нельзя — легко получить чужие значения.
 *
 * Смещения взяты из musb_regs.h ядра Linux. Проверяем их тем же способом,
 * что и раньше: если загрузчик оставил контроллер настроенным, значения
 * будут осмысленными, а не нулями и не сплошными единицами.
 */
#include "usb.h"
#include "io.h"
#include "print.h"
#include "spinlock.h"
#include "smp.h"

#if defined(BOARD_MERLIN)
#include "soc/mt6768.h"

#define USB_BASE        MT_USB0_BASE

/* Общие регистры. Ширина разная: часть однобайтовые, часть двухбайтовые —
 * обращаться не той шириной к Device-памяти нельзя. */
#define MUSB_FADDR      0x00    /* 8  адрес, назначенный хостом */
#define MUSB_POWER      0x01    /* 8  питание и режим скорости  */
#define MUSB_INTRTX     0x02    /* 16 */
#define MUSB_INTRRX     0x04    /* 16 */
#define MUSB_INTRUSB    0x0A    /* 8  события шины              */
#define MUSB_INTRUSBE   0x0B    /* 8  какие события разрешены   */
#define MUSB_FRAME      0x0C    /* 16 номер кадра от хоста      */
#define MUSB_INDEX      0x0E    /* 8  окно конечной точки       */
#define MUSB_DEVCTL     0x60    /* 8  сессия, роль              */

/* Биты POWER */
#define POWER_ENSUSPEND (1 << 0)
#define POWER_SUSPENDM  (1 << 1)
#define POWER_RESUME    (1 << 2)
#define POWER_RESET     (1 << 3)
#define POWER_HSMODE    (1 << 4)    /* договорились о высокой скорости */
#define POWER_HSENAB    (1 << 5)    /* высокая скорость разрешена      */
#define POWER_SOFTCONN  (1 << 6)    /* подтяжка на линии: «я подключён» */

/* Биты DEVCTL */
#define DEVCTL_SESSION  (1 << 0)
#define DEVCTL_HM       (1 << 2)    /* режим хоста                     */
#define DEVCTL_BDEVICE  (1 << 7)    /* мы устройство, а не хост        */

/*
 * Заявить о себе на шине.
 *
 * Подтяжка на линии D+ — это и есть физическое «я подключён» для хоста:
 * пока её нет, компьютер видит пустой порт, что мы и наблюдали. Включается
 * одним битом SOFTCONN.
 *
 * Проверка получается неожиданно дешёвой: даже если дальше мы не ответим
 * ни на один запрос, Windows покажет неизвестное устройство — а это уже
 * доказательство, что физический уровень работает. И увидеть это можно
 * со стороны компьютера, не фотографируя экран.
 */
/*
 * В каком состоянии блоки физического уровня.
 *
 * Кандидатов два, и по дереву не понять, какой из них наш:
 *   0x11210000  узел usb1p_sif — «системный интерфейс» одного порта
 *   0x11CC0000  второй диапазон самого узла usb0
 *
 * Отличаем по простому признаку: обесточенный блок читается нулями.
 * Тот, где есть содержимое, и есть живой.
 */
void usb_phy_probe(void)
{
    u32 a = 0, b = 0;

    for (u32 o = 0; o < 0x1000; o += 4) {
        if (mmio_read32(0x11210000UL + o))
            a++;
        if (mmio_read32(0x11CC0000UL + o))
            b++;
    }
    kprintf("PHY 11210000: %u, 11CC0000: %u ИЗ 1024\n", a, b);

    /* Ненулевых мало, поэтому выписываем их все: по расположению
     * регистров раскладку опознать проще, чем угадывать смещения. */
    kprintf("ЖИВЫЕ РЕГИСТРЫ 11CC0000:\n");
    {
        u32 shown = 0;

        for (u32 o = 0; o < 0x1000; o += 4) {
            u32 v = mmio_read32(0x11CC0000UL + o);

            if (!v)
                continue;
            kprintf(" %03x=%08x", o, v);
            if (++shown % 3 == 0)
                kprintf("\n");
        }
        if (shown % 3)
            kprintf("\n");
    }
}

void usb_connect(void)
{
    u8 power = mmio_read8(USB_BASE + MUSB_POWER);

    power |= POWER_SOFTCONN | POWER_HSENAB;
    mmio_write8(USB_BASE + MUSB_POWER, power);
    dsb();

    kprintf("USB: ПОДТЯЖКА ВКЛЮЧЕНА, POWER %x\n",
            mmio_read8(USB_BASE + MUSB_POWER));
}

/*
 * Следить за шиной и рассказывать, что на ней происходит.
 *
 * Хост, увидев подтяжку, обязан выдать сброс шины, и это отразится в
 * INTRUSB. Появление сброса — верный признак, что нас заметили; дальше
 * пойдут запросы дескрипторов на нулевой конечной точке.
 */
void usb_watch(u32 seconds)
{
    u64 end = read_cntpct() + read_cntfrq() * seconds;
    u8 seen = 0;
    u32 resets = 0, ep0 = 0;

    kprintf("USB: СЛЕЖУ ЗА ШИНОЙ %u СЕК...\n", seconds);
    while (read_cntpct() < end) {
        u8  ev = mmio_read8(USB_BASE + MUSB_INTRUSB);
        u16 tx = mmio_read16(USB_BASE + MUSB_INTRTX);

        if (ev) {
            seen |= ev;
            if (ev & 0x04)
                resets++;
        }
        if (tx & 1)
            ep0++;
    }

    kprintf("USB: СОБЫТИЯ %x, СБРОСОВ %u, EP0 %u\n", seen, resets, ep0);
    kprintf("USB: FADDR %x POWER %x DEVCTL %x\n",
            mmio_read8(USB_BASE + MUSB_FADDR),
            mmio_read8(USB_BASE + MUSB_POWER),
            mmio_read8(USB_BASE + MUSB_DEVCTL));
}

/* --- Физический уровень USB 2.0 ---
 * Блок найден опытом: живые регистры начинаются с 0x800 от второго
 * диапазона узла usb0. Это стандартная раскладка MediaTek.
 *
 * Ключевое наблюдение с устройства: DTM0 = 0x02040000, то есть выставлен
 * FORCE_SUSPENDM при сброшенном RG_SUSPENDM — загрузчик принудительно
 * усыпил физический уровень перед передачей управления. Отсюда и мёртвая
 * линия: контроллер подтяжку включает, а тянуть нечем.
 *
 * Имена битов из phy-mtk-tphy.c ядра Linux.
 */
#define PHY_BASE            (0x11CC0000UL + 0x800)

#define U2_PHYACR0          0x00
#define U2_PHYACR6          0x18
#define U2_PHYDTM0          0x68
#define U2_PHYDTM1          0x6C

/* PHYACR0 */
#define PA0_RG_INTR_EN      (1U << 5)
/* PHYACR6 */
#define PA6_RG_BC11_SW_EN   (1U << 23)  /* определение зарядника: мешает   */
#define PA6_RG_VBUSCMP_EN   (1U << 20)  /* компаратор VBUS                 */
/* DTM0 */
#define DTM0_FORCE_UART_EN  (1U << 26)
#define DTM0_FORCE_DATAIN   (1U << 23)
#define DTM0_FORCE_DM_PD    (1U << 21)
#define DTM0_FORCE_DP_PD    (1U << 20)
#define DTM0_FORCE_XCVRSEL  (1U << 19)
#define DTM0_FORCE_SUSPENDM (1U << 18)
#define DTM0_FORCE_TERMSEL  (1U << 17)
#define DTM0_RG_DATAIN      (0xFU << 10)
#define DTM0_RG_XCVRSEL     (0x3U << 4)
#define DTM0_RG_SUSPENDM    (1U << 3)
#define DTM0_RG_TERMSEL     (1U << 2)
/* DTM1 */
#define DTM1_RG_UART_EN     (1U << 16)
#define DTM1_FORCE_VBUSVALID (1U << 13)
#define DTM1_FORCE_SESSEND  (1U << 12)
#define DTM1_FORCE_AVALID   (1U << 10)
#define DTM1_FORCE_IDDIG    (1U << 9)
#define DTM1_RG_VBUSVALID   (1U << 5)
#define DTM1_RG_SESSEND     (1U << 4)
#define DTM1_RG_AVALID      (1U << 2)
#define DTM1_RG_IDDIG       (1U << 1)

/*
 * Разбудить физический уровень и объявить себя устройством.
 *
 * Порядок важен. Сперва снимаем принудительный сон и все режимы, которыми
 * загрузчик мог удерживать линии (UART поверх USB, ручное управление
 * подтяжками). Потом включаем сам приёмопередатчик. И только в конце
 * подделываем состояние разъёма: сообщаем контроллеру, что кабель вставлен
 * и мы на нём — устройство, а не хост. Без этого он не начнёт сессию,
 * сколько подтяжку ни включай.
 */
void usb_phy_on(void)
{
    u32 v;

    /* 1. Никакого принудительного сна и никаких ручных режимов */
    v = mmio_read32(PHY_BASE + U2_PHYDTM0);
    v &= ~(DTM0_FORCE_UART_EN | DTM0_FORCE_DATAIN | DTM0_FORCE_DM_PD |
           DTM0_FORCE_DP_PD | DTM0_FORCE_XCVRSEL | DTM0_FORCE_SUSPENDM |
           DTM0_FORCE_TERMSEL | DTM0_RG_DATAIN | DTM0_RG_XCVRSEL |
           DTM0_RG_TERMSEL);
    v |= DTM0_RG_SUSPENDM;              /* «не спать» */
    mmio_write32(PHY_BASE + U2_PHYDTM0, v);

    /* 2. UART поверх линий USB отключаем: он их занимает */
    v = mmio_read32(PHY_BASE + U2_PHYDTM1);
    v &= ~DTM1_RG_UART_EN;
    mmio_write32(PHY_BASE + U2_PHYDTM1, v);

    /* 3. Включаем приёмопередатчик и компаратор VBUS, выключаем
     *    определение зарядника — оно тянет линии по-своему */
    v = mmio_read32(PHY_BASE + U2_PHYACR0);
    v |= PA0_RG_INTR_EN;
    mmio_write32(PHY_BASE + U2_PHYACR0, v);

    v = mmio_read32(PHY_BASE + U2_PHYACR6);
    v &= ~PA6_RG_BC11_SW_EN;
    v |= PA6_RG_VBUSCMP_EN;
    mmio_write32(PHY_BASE + U2_PHYACR6, v);

    /* 4. Подделываем состояние разъёма: кабель вставлен, мы устройство.
     *    Иначе контроллер будет ждать событий от определителя роли,
     *    которого мы не поднимали. */
    v = mmio_read32(PHY_BASE + U2_PHYDTM1);
    v |= DTM1_FORCE_VBUSVALID | DTM1_RG_VBUSVALID |
         DTM1_FORCE_AVALID   | DTM1_RG_AVALID    |
         DTM1_FORCE_SESSEND  | DTM1_FORCE_IDDIG  | DTM1_RG_IDDIG;
    v &= ~DTM1_RG_SESSEND;
    mmio_write32(PHY_BASE + U2_PHYDTM1, v);
    dsb();

    kprintf("PHY: DTM0 %x DTM1 %x ACR6 %x\n",
            mmio_read32(PHY_BASE + U2_PHYDTM0),
            mmio_read32(PHY_BASE + U2_PHYDTM1),
            mmio_read32(PHY_BASE + U2_PHYACR6));
}

void usb_probe(void)
{
    u8  faddr = mmio_read8(USB_BASE + MUSB_FADDR);
    u8  power = mmio_read8(USB_BASE + MUSB_POWER);
    u8  devctl = mmio_read8(USB_BASE + MUSB_DEVCTL);
    u8  intrusb = mmio_read8(USB_BASE + MUSB_INTRUSB);
    u8  intrusbe = mmio_read8(USB_BASE + MUSB_INTRUSBE);
    u16 frame = mmio_read16(USB_BASE + MUSB_FRAME);
    u32 nz = 0;

    /* Сколько вообще ненулевого в блоке: отличает «не тактируется»
     * от «настроен, но простаивает» */
    for (u32 o = 0; o < 0x200; o += 4)
        if (mmio_read32(USB_BASE + o))
            nz++;

    kprintf("USB БАЗА 0x%08lx, НЕНУЛЕВЫХ %u ИЗ 128\n", (u64)USB_BASE, nz);
    kprintf("  FADDR %x  FRAME %x\n", faddr, frame);
    kprintf("  POWER %x:%s%s%s\n", power,
            (power & POWER_SOFTCONN) ? " ПОДКЛЮЧЕН" : " ОТКЛЮЧЕН",
            (power & POWER_HSENAB)   ? " HS-РАЗРЕШЕН" : "",
            (power & POWER_HSMODE)   ? " HS-АКТИВЕН" : "");
    kprintf("  DEVCTL %x:%s%s\n", devctl,
            (devctl & DEVCTL_SESSION) ? " СЕССИЯ" : " НЕТ СЕССИИ",
            (devctl & DEVCTL_HM) ? " ХОСТ" : " УСТРОЙСТВО");
    kprintf("  INTRUSB %x РАЗРЕШЕНО %x\n", intrusb, intrusbe);
}


/* ================== Перечисление устройства ==================
 *
 * Хост уже нас видит и шлёт запросы на нулевую конечную точку, но пока
 * никто не отвечает — Windows показывает «сбой запроса дескриптора».
 * Отвечать на эти запросы и значит «перечислиться».
 *
 * Представляемся последовательным портом (класс CDC-ACM): для него в
 * Windows есть встроенный драйвер, и телефон появится как COM-порт сам,
 * без установки чего-либо руками.
 */

#define MUSB_CSR0           0x12    /* 16 бит */
#define MUSB_COUNT0         0x18    /* 8  бит */
#define MUSB_TXMAXP         0x10
#define MUSB_TXCSR          0x12    /* тот же адрес, когда INDEX не ноль */
#define MUSB_FIFO(ep)       (0x20 + 4 * (ep))

#define CSR0_RXPKTRDY       0x0001
#define CSR0_TXPKTRDY       0x0002
#define CSR0_SENTSTALL      0x0004
#define CSR0_DATAEND        0x0008
#define CSR0_SETUPEND       0x0010
#define CSR0_SENDSTALL      0x0020
#define CSR0_SVDRXPKTRDY    0x0040
#define CSR0_SVDSETUPEND    0x0080

#define TXCSR_TXPKTRDY      0x0001
#define TXCSR_FLUSHFIFO     0x0008
#define TXCSR_CLRDATATOG    0x0040

#define EP0_MAXP            64
#define EP_BULK             2
#define EP_BULK_MAXP        512

static const u8 desc_device[18] = {
    18, 0x01,
    0x00, 0x02,                 /* USB 2.00 */
    0x02, 0x00, 0x00,           /* класс CDC: система сама подберёт
                                 * драйвер последовательного порта */
    EP0_MAXP,
    0xC4, 0x0B,                 /* производитель */
    0x01, 0x56,                 /* изделие */
    0x00, 0x01,
    1, 2, 0,
    1
};

#define CFG_TOTAL 67

static const u8 desc_config[CFG_TOTAL] = {
    9, 0x02, CFG_TOTAL, 0x00, 2, 1, 0, 0xC0, 50,

    /* Интерфейс 0: управляющая часть порта */
    9, 0x04, 0, 0, 1, 0x02, 0x02, 0x01, 0,
    5, 0x24, 0x00, 0x10, 0x01,
    5, 0x24, 0x01, 0x00, 0x01,
    4, 0x24, 0x02, 0x02,
    5, 0x24, 0x06, 0x00, 0x01,
    /* Точка уведомлений: по спецификации обязана быть, мы её не используем */
    7, 0x05, 0x81, 0x03, 0x08, 0x00, 0xFF,

    /* Интерфейс 1: данные */
    9, 0x04, 1, 0, 2, 0x0A, 0x00, 0x00, 0,
    7, 0x05, EP_BULK, 0x02, 0x00, 0x02, 0,
    7, 0x05, 0x80 | EP_BULK, 0x02, 0x00, 0x02, 0
};

/* Строки в UTF-16. Символы записаны кодами, чтобы исходник не зависел
 * от кодировки файла. */
static const u8 desc_str0[4] = { 4, 0x03, 0x09, 0x04 };
static const u8 desc_str1[16] = {
    16, 0x03,
    0x56,0, 0x45,0, 0x4C,0, 0x4F,0, 0x2D,0, 0x4F,0, 0x53,0
};
static const u8 desc_str2[32] = {
    32, 0x03,
    0x56,0, 0x45,0, 0x4C,0, 0x4F,0, 0x2D,0, 0x4F,0, 0x53,0, 0x20,0,
    0x43,0, 0x4F,0, 0x4E,0, 0x53,0, 0x4F,0, 0x4C,0, 0x45,0
};

/*
 * Замок на контроллер.
 *
 * Обслуживание вызывается откуда угодно: из вывода, из пауз, из точки
 * залипания, а после запуска планировщика — ещё и с восьми ядер сразу.
 * У MUSB регистры конечных точек видны через одно окно (регистр INDEX),
 * поэтому два ядра, работающие одновременно, гарантированно подменят
 * друг другу выбранную точку и запишут данные не туда.
 *
 * Прерывания при этом запрещаем: вывод вызывается и из обработчиков.
 */
static struct spinlock usb_lock = SPINLOCK_INIT("usb");

static void ring_drain_locked(void);     /* определена ниже, в консоли */

static const u8 *ep0_tx;
static u32 ep0_tx_left;
static u8  ep0_addr_pending;
static u64 ep0_addr_deadline;       /* когда можно применить адрес */
static int usb_configured;

/*
 * Открыт ли порт на той стороне.
 *
 * Хост сообщает об этом отдельным запросом класса: SET_CONTROL_LINE_STATE
 * с поднятым битом DTR приходит ровно в момент открытия порта терминалом
 * и снимается при закрытии. До этого момента всё, что мы отправим,
 * драйвер на компьютере просто выбрасывает — порт ещё никем не читается.
 *
 * Пока сигнала нет, копим в кольце и молчим. Именно из-за отсутствия
 * этой проверки начало загрузочного отчёта пропадало каждый раз: ядро
 * добросовестно отдавало его в пустоту, а терминал подключался секундой
 * позже и заставал уже середину.
 */
static int usb_host_open;

u32 usb_setup_count;

static void fifo_write(u32 ep, const u8 *d, u32 n)
{
    for (u32 i = 0; i < n; i++)
        mmio_write8(USB_BASE + MUSB_FIFO(ep), d[i]);
}

static void fifo_read(u32 ep, u8 *d, u32 n)
{
    for (u32 i = 0; i < n; i++)
        d[i] = mmio_read8(USB_BASE + MUSB_FIFO(ep));
}


/* Отдать очередную порцию ответа. Хост забирает пакетами по 64 байта. */
static void ep0_send_chunk(void)
{
    u32 n = ep0_tx_left > EP0_MAXP ? EP0_MAXP : ep0_tx_left;
    u16 csr = CSR0_TXPKTRDY;

    fifo_write(0, ep0_tx, n);
    ep0_tx += n;
    ep0_tx_left -= n;

    /* Короткий пакет сам по себе означает конец посылки */
    if (ep0_tx_left == 0 || n < EP0_MAXP)
        csr |= CSR0_DATAEND;
    mmio_write16(USB_BASE + MUSB_CSR0, csr);
}

static void ep0_stall(void)
{
    mmio_write16(USB_BASE + MUSB_CSR0, CSR0_SENDSTALL | CSR0_SVDRXPKTRDY);
}

/* Точка данных нужна только после того, как хост выбрал конфигурацию */
static void bulk_setup(void)
{
    /* Точка уведомлений. Сами мы её не используем, но объявили в
     * дескрипторе, и драйвер порта будет её опрашивать. Ненастроенная
     * точка отвечает мусором, поэтому настроить обязаны. */
    mmio_write8(USB_BASE + MUSB_INDEX, 1);
    mmio_write16(USB_BASE + MUSB_TXMAXP, 8);
    mmio_write16(USB_BASE + MUSB_TXCSR, TXCSR_FLUSHFIFO | TXCSR_CLRDATATOG);

    /* Точка данных: ради неё всё и затевалось */
    mmio_write8(USB_BASE + MUSB_INDEX, EP_BULK);
    mmio_write16(USB_BASE + MUSB_TXMAXP, EP_BULK_MAXP);
    mmio_write16(USB_BASE + MUSB_TXCSR, TXCSR_FLUSHFIFO | TXCSR_CLRDATATOG);

    mmio_write8(USB_BASE + MUSB_INDEX, 0);
}

static const u8 val_one = 1;
static const u8 val_zero = 0;
static const u8 line_coding[7] = { 0x00, 0xC2, 0x01, 0x00, 0x00, 0x00, 0x08 };

static void ep0_setup(const u8 *p)
{
    u8  type = p[0], req = p[1];
    u16 val  = (u16)(p[2] | (p[3] << 8));
    u16 len  = (u16)(p[6] | (p[7] << 8));
    const u8 *d = 0;
    u32 dlen = 0;

    usb_setup_count++;

    if ((type & 0x60) == 0x00) {
        switch (req) {
        case 0x06:
            switch (val >> 8) {
            case 1: d = desc_device; dlen = sizeof(desc_device); break;
            case 2: d = desc_config; dlen = sizeof(desc_config); break;
            case 3:
                switch (val & 0xFF) {
                case 0: d = desc_str0; dlen = sizeof(desc_str0); break;
                case 1: d = desc_str1; dlen = sizeof(desc_str1); break;
                case 2: d = desc_str2; dlen = sizeof(desc_str2); break;
                }
                break;
            }
            break;
        case 0x05:
            /* Адрес применяем ТОЛЬКО после подтверждения запроса: до этого
             * хост ещё говорит с нами по нулевому адресу. */
            ep0_addr_pending = (u8)(val & 0x7F);
            /*
             * Адрес меняем не сразу. По спецификации устройство обязано
             * полностью завершить подтверждение ЭТОГО запроса на СТАРОМ
             * адресе, и только потом перейти на новый. Если переключиться
             * раньше, хост ждёт ответа по старому адресу, а мы уже
             * отвечаем по новому — он объявляет «сбой задания адреса».
             *
             * Отсчитываем миллисекунду: подтверждение за это время
             * гарантированно уходит, а спецификация даёт устройству
             * на переход куда больше.
             */
            ep0_addr_deadline = read_cntpct() + read_cntfrq() / 1000;
            mmio_write16(USB_BASE + MUSB_CSR0, CSR0_SVDRXPKTRDY | CSR0_DATAEND);
            return;
        case 0x09:
            usb_configured = (val != 0);
            if (usb_configured) {
                bulk_setup();
                /*
                 * Отдаём сразу, без задержки.
                 *
                 * Пробовали придержать буфер на две секунды, чтобы терминал
                 * успел открыть порт и поймал загрузочный отчёт целиком. Не
                 * вышло: ядро всё это время продолжает печатать, кольцо
                 * переполняется, и вытесняется как раз начало — ровно то,
                 * ради чего задержка и делалась.
                 *
                 * Начало загрузки пока теряется. Это неприятно, но терпимо:
                 * всё, что печатается после подключения терминала, видно
                 * полностью. Правильное решение — отдельный буфер под
                 * загрузочный отчёт, который не вытесняется живым выводом.
                 */
            }
            mmio_write16(USB_BASE + MUSB_CSR0, CSR0_SVDRXPKTRDY | CSR0_DATAEND);
            return;
        case 0x08: d = &val_one;  dlen = 1; break;
        case 0x0A: d = &val_zero; dlen = 1; break;
        }
    } else if ((type & 0x60) == 0x20) {
        /* Запросы класса порта: скорость, чётность, состояние линий.
         * Провода нет, данные идут по USB, поэтому они нам безразличны.
         * Но молча подтвердить обязаны, иначе система сочтёт порт
         * неисправным и откажется его открывать. */
        if (type & 0x80) {
            d = line_coding; dlen = sizeof(line_coding);
        } else if (len) {
            /*
             * У запроса есть стадия данных: следом придут ещё байты
             * (например, семь байт параметров скорости). Подтверждаем
             * только сам запрос, БЕЗ признака завершения — иначе хост
             * увидит обрыв посылки и объявит устройство неисправным.
             * Данные примет и выбросит общий обработчик ниже.
             */
            mmio_write16(USB_BASE + MUSB_CSR0, CSR0_SVDRXPKTRDY);
            return;
        } else {
            /* SET_CONTROL_LINE_STATE: младший бит — DTR, «порт открыт».
             * Единственный класс-запрос, который нам не безразличен. */
            if (req == 0x22)
                usb_host_open = (val & 1) != 0;
            /* Данных нет — завершаем сразу */
            mmio_write16(USB_BASE + MUSB_CSR0, CSR0_SVDRXPKTRDY | CSR0_DATAEND);
            return;
        }
    }

    if (!d) {
        ep0_stall();
        return;
    }

    if (dlen > len)
        dlen = len;
    ep0_tx = d;
    ep0_tx_left = dlen;

    mmio_write16(USB_BASE + MUSB_CSR0, CSR0_SVDRXPKTRDY);
    ep0_send_chunk();
}

/*
 * Один шаг обслуживания нулевой точки.
 *
 * Вызывать почаще: пока мы не ответим, хост ждёт, а по истечении времени
 * объявит устройство неисправным.
 */
static void usb_poll_locked(void)
{
    u16 csr;
    u8 setup[8];

    mmio_write8(USB_BASE + MUSB_INDEX, 0);
    csr = mmio_read16(USB_BASE + MUSB_CSR0);

    if (csr & CSR0_SETUPEND) {          /* хост оборвал посылку */
        mmio_write16(USB_BASE + MUSB_CSR0, CSR0_SVDSETUPEND);
        ep0_tx_left = 0;
        csr = mmio_read16(USB_BASE + MUSB_CSR0);
    }

    if (csr & CSR0_SENTSTALL)
        mmio_write16(USB_BASE + MUSB_CSR0, 0);

    if (csr & CSR0_RXPKTRDY) {
        u8 n = mmio_read8(USB_BASE + MUSB_COUNT0);

        if (n == 8) {
            fifo_read(0, setup, 8);
            ep0_setup(setup);
        } else {
            u8 junk;

            for (u8 i = 0; i < n; i++)
                fifo_read(0, &junk, 1);
            mmio_write16(USB_BASE + MUSB_CSR0, CSR0_SVDRXPKTRDY | CSR0_DATAEND);
        }
        return;
    }

    if (ep0_tx_left && !(csr & CSR0_TXPKTRDY)) {
        ep0_send_chunk();
        return;
    }

    /* Подтверждение ушло по проводу — теперь можно перейти на новый адрес */
    if (ep0_addr_pending && read_cntpct() >= ep0_addr_deadline &&
        !(csr & CSR0_TXPKTRDY) && !ep0_tx_left) {
        mmio_write8(USB_BASE + MUSB_FADDR, ep0_addr_pending);
        ep0_addr_pending = 0;
    }
}

/*
 * Контроллером владеет только загрузочное ядро.
 *
 * Первая версия обслуживала USB откуда угодно, и это сломало запуск
 * дополнительных ядер: они вызывают паузу при старте, пауза лезла в
 * обслуживание, а обслуживание — в замок контроллера. Ядра дрались за
 * него друг с другом ещё до того, как успевали подняться, и вместо
 * восьми в системе оставалось два.
 *
 * У MUSB регистры конечных точек и так видны через одно окно, поэтому
 * единственный владелец — не ограничение, а естественный порядок.
 */
void usb_poll(void)
{
    u64 flags;

    if (cpu_id() != 0)
        return;

    flags = spin_lock_irq(&usb_lock);
    usb_poll_locked();
    /* Отдаём накопленное здесь, а не в самой печати: usb_send_locked
     * в ожидании вызывает обслуживание нулевой точки, и вызов отдачи
     * оттуда же ушёл бы в бесконечную рекурсию. */
    ring_drain_locked();
    spin_unlock_irq(&usb_lock, flags);
}

/*
 * До каких пор ждать хоста.
 *
 * Ожидание здесь идёт под замком USB и с запрещёнными прерываниями:
 * пока мы ждём, это ядро не делает ничего, а остальные встают на замке,
 * едва им понадобится что-нибудь напечатать. То есть цена ожидания —
 * не «медленнее печать», а «система не отвечает».
 *
 * Раньше предел был в оборотах цикла — двести тысяч, — и это оказалось
 * почти вечностью: закрытый на компьютере терминал перестаёт забирать
 * данные, и телефон замирал целиком, пока кто-нибудь печатал. Теперь
 * предел во времени и короткий, а после неудачи мы вообще не пытаемся
 * отдавать ещё десятую долю секунды.
 *
 * Правило прежнее и записано было верно: зависнуть в выводе хуже, чем
 * потерять строку. Не хватало только строгости в его исполнении.
 */
#define TX_WAIT_MS      2
#define TX_BACKOFF_MS   100

static u64 tx_stalled_until;

/* Отправить байты хосту. До перечисления молча ничего не делает. */
static void usb_send_locked(const u8 *data, u32 len)
{

    while (len) {
        u32 n = len > EP_BULK_MAXP ? EP_BULK_MAXP : len;
        u64 deadline = read_cntpct() + read_cntfrq() * TX_WAIT_MS / 1000;
        int late = 0;

        mmio_write8(USB_BASE + MUSB_INDEX, EP_BULK);
        /* Пока ждём, продолжаем отвечать хосту: он в это время может
         * прислать управляющий запрос, и молчание он не простит. */
        while (mmio_read16(USB_BASE + MUSB_TXCSR) & TXCSR_TXPKTRDY) {
            if (read_cntpct() > deadline) {
                late = 1;
                break;
            }
            mmio_write8(USB_BASE + MUSB_INDEX, 0);
            usb_poll_locked();
            mmio_write8(USB_BASE + MUSB_INDEX, EP_BULK);
        }
        if (late) {
            /* Хост не забирает данные. Молча выбрасываем и какое-то
             * время даже не пробуем: каждая попытка стоит остановки
             * всех ядер, которым нужно печатать. */
            mmio_write8(USB_BASE + MUSB_INDEX, 0);
            tx_stalled_until = read_cntpct() +
                               read_cntfrq() * TX_BACKOFF_MS / 1000;
            return;
        }
        fifo_write(EP_BULK, data, n);
        mmio_write16(USB_BASE + MUSB_TXCSR, TXCSR_TXPKTRDY);
        mmio_write8(USB_BASE + MUSB_INDEX, 0);

        data += n;
        len -= n;
    }
}

void usb_send(const u8 *data, u32 len)
{
    u64 flags;

    /* Печатать могут все ядра, но в контроллер лезет только загрузочное.
     * Остальным строка просто не достаётся — на экране и в UART она
     * всё равно останется. */
    if (!usb_configured || !usb_host_open || cpu_id() != 0)
        return;
    flags = spin_lock_irq(&usb_lock);
    usb_send_locked(data, len);
    spin_unlock_irq(&usb_lock, flags);
}

int usb_ready(void) { return usb_configured; }

/* ================== Консоль поверх USB ==================
 *
 * Печатать может любое ядро, а в контроллер лезет только нулевое —
 * иначе восемь ядер дерутся за окно выбора конечной точки. Поэтому
 * между ними стоит кольцевой буфер: печать кладёт в него байты откуда
 * угодно, а нулевое ядро при очередном обслуживании отдаёт накопленное.
 *
 * Первая версия просто выбрасывала строки с остальных ядер. До запуска
 * планировщика это было незаметно, а после — пропал почти весь вывод:
 * задачи печатают с того ядра, на котором их застал квант.
 */
/*
 * Буфер держим большим намеренно: консоль подключается через несколько
 * секунд после старта, а самое интересное ядро печатает в первые
 * мгновения — карту памяти, состояние GIC, отчёт о запуске ядер. Копим
 * всё с первой строки и отдаём разом, как только появится кому.
 */
#define CON_RING 16384                  /* степень двойки: маска вместо деления */

static u8  ring[CON_RING];
static u32 ring_head, ring_tail;

static void ring_put(u8 c)
{
    u32 next = (ring_head + 1) & (CON_RING - 1);

    /* Переполнение: теряем самое старое. Лучше свежий вывод без начала,
     * чем застрявшая печать в ожидании места. */
    if (next == ring_tail)
        ring_tail = (ring_tail + 1) & (CON_RING - 1);
    ring[ring_head] = c;
    ring_head = next;
}

void usb_putc(char c)
{
    u64 flags;

    /* Копим независимо от того, подключён ли хост: иначе весь загрузочный
     * отчёт пропадает, а он и есть самое ценное. */
    flags = spin_lock_irq(&usb_lock);
    if (c == 10)
        ring_put(13);                   /* терминалы ждут пару CR+LF */
    ring_put((u8)c);
    spin_unlock_irq(&usb_lock, flags);
}

/* Отдать накопленное. Вызывается под замком и только с нулевого ядра. */
static void ring_drain_locked(void)
{
    u8 buf[64];

    /* Хост недавно не забрал пакет — не тратим на него время вовсе */
    if (tx_stalled_until && read_cntpct() < tx_stalled_until)
        return;
    tx_stalled_until = 0;

    /*
     * За один раз отдаём не больше восьми пакетов.
     *
     * Отдача идёт под замком и с закрытыми прерываниями, а кольцо — это
     * шестнадцать килобайт. Вычерпать его целиком означало бы удержать
     * все ядра на время двухсот пятидесяти посылок. Отдача вызывается по
     * тику, сто раз в секунду: восьми пакетов за раз хватает на
     * пятьдесят килобайт в секунду, а это больше, чем мы печатаем.
     */
    for (u32 packet = 0; packet < 8 && ring_tail != ring_head; packet++) {
        u32 n = 0;

        while (n < sizeof(buf) && ring_tail != ring_head) {
            buf[n++] = ring[ring_tail];
            ring_tail = (ring_tail + 1) & (CON_RING - 1);
        }
        usb_send_locked(buf, n);

        if (tx_stalled_until)   /* хост отвалился — дальше не пытаемся */
            return;
    }
}

void usb_flush(void)
{
    u64 flags;

    if (!usb_configured || !usb_host_open || cpu_id() != 0)
        return;
    flags = spin_lock_irq(&usb_lock);
    ring_drain_locked();
    spin_unlock_irq(&usb_lock, flags);
}

#else
void usb_probe(void) { }
void usb_connect(void) { }
void usb_phy_probe(void) { }
void usb_phy_on(void) { }
void usb_poll(void) { }
void usb_send(const u8 *d, u32 n) { (void)d; (void)n; }
int  usb_ready(void) { return 0; }
void usb_putc(char c) { (void)c; }
void usb_flush(void) { }
void usb_watch(u32 s) { (void)s; }
#endif
