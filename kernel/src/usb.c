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

#else
void usb_probe(void) { }
void usb_connect(void) { }
void usb_phy_probe(void) { }
void usb_phy_on(void) { }
void usb_watch(u32 s) { (void)s; }
#endif
