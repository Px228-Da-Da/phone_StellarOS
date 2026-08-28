#ifndef SOC_MT6769_H
#define SOC_MT6769_H
/*
 * Карта регистров MediaTek MT6769Z (Helio G85), устройство merlin.
 *
 * ВНИМАНИЕ. MediaTek не публикует datasheet. Значения ниже взяты из
 * mt6765.dtsi / mt6768.dtsi (одно семейство SoC). Помеченное TODO-VERIFY
 * обязательно сверить с DTS официального ядра Xiaomi для merlin
 * (см. docs/00-device.md) ПЕРЕД прошивкой на телефон.
 */

/* --- UART ---
 * MTK ставит контроллеры, совместимые с 8250, но с шагом регистров 4 байта.
 * UART0 физически выведен на тест-пойнты платы либо на USB-D+/D- через
 * переключатель — до пайки считаем этот канал недоступным и полагаемся на экран. */
#define MT_UART0_BASE       0x11002000UL    /* TODO-VERIFY по mt6768.dtsi */
#define MT_UART1_BASE       0x11003000UL    /* TODO-VERIFY */

/* Регистры 8250 (умножены на 4: UART_REG_SHIFT = 2) */
#define UART_RBR_THR        0x00            /* данные rx/tx            */
#define UART_IER            0x04
#define UART_FCR            0x08
#define UART_LCR            0x0C
#define UART_LSR            0x14            /* бит 5 = THR пуст        */
#define UART_LSR_THRE       (1 << 5)
#define UART_LSR_DR         (1 << 0)        /* есть принятый байт      */

/* --- GIC ---
 * MT6768/6769 несут GIC-400 (GICv2) в аппаратном виде, но ATF может
 * выставлять GICv3-совместимость. Определяем на этапе GIC-драйвера. */
#define MT_GICD_BASE        0x0C000000UL    /* TODO-VERIFY */
#define MT_GICC_BASE        0x0C002000UL    /* TODO-VERIFY (GICv2 CPU iface) */

/* --- Дисплей ---
 * Панель merlin: 1080x2340. LK оставляет фреймбуфер включённым
 * (continuous splash), но его адрес выделяется динамически из верхней
 * области DRAM. Ищем его в DTB (узел /chosen или /reserved-memory),
 * а не хардкодим. Константы ниже — только геометрия. */
#define MERLIN_FB_WIDTH     1080
#define MERLIN_FB_HEIGHT    2340
#define MERLIN_FB_BPP       32              /* обычно BGRA8888 */

/* --- Таймер ---
 * Частота системного счётчика читается из CNTFRQ_EL0; у MTK это 13 МГц. */
#define MT_ARCH_TIMER_IRQ   30              /* PPI 14 -> INTID 30, non-secure phys */

#endif
