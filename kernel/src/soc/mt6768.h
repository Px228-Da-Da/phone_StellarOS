#ifndef SOC_MT6768_H
#define SOC_MT6768_H
/*
 * Карта регистров MediaTek MT6768 (Helio G85), устройство merlinnfc
 * (Redmi Note 9 NFC, M2003J15SC).
 *
 * ВСЕ адреса ниже СНЯТЫ С ЖИВОГО УСТРОЙСТВА: имена узлов в
 * /proc/device-tree содержат базовые адреса (`serial@11002000`).
 * Полный список — device-info/dt-nodes.txt. Это не догадки.
 *
 * Замечание: ro.board.platform = mt6768, хотя маркетинговое имя SoC
 * иногда пишут как MT6769. Ориентируемся на mt6768.
 */

/* --- UART (8250-совместимый, шаг регистров 4 байта) ---
 * Подтверждено: serial@11002000, serial@11003000 */
#define MT_UART0_BASE       0x11002000UL
#define MT_UART1_BASE       0x11003000UL

#define UART_RBR_THR        0x00            /* данные rx/tx            */
#define UART_IER            0x04
#define UART_FCR            0x08
#define UART_LCR            0x0C
#define UART_LSR            0x14
#define UART_LSR_THRE       (1 << 5)        /* THR пуст, можно слать   */
#define UART_LSR_DR         (1 << 0)        /* есть принятый байт      */

/* --- Контроллер прерываний ---
 * Узел называется gic500@0c000000 => это GIC-500, то есть GICv3.
 * ВАЖНО для этапа 3: GICv3 настраивается через системные регистры
 * ICC_* (msr/mrs), а не через MMIO CPU-интерфейс как GICv2.
 * gic_cpu@0c400000 — блок GIC CPU wakeup, не путать с GICC из v2. */
#define MT_GICD_BASE        0x0C000000UL    /* distributor             */
/* TODO-VERIFY: имя узла даёт только базу дистрибьютора. Адрес редистрибьюторов
 * взят по раскладке GIC-500 в ядрах MediaTek mt6765/mt6768 (dist 0x0c000000
 * размером 256 КБ, redist 0x0c100000 размером 2 МБ). Сверить по merlin.dts
 * сразу после разблокировки — это единственное здесь непроверенное число. */
#define MT_GICR_BASE        0x0C100000UL    /* redistributors, шаг 128 КБ */
#define MT_GIC_CPU_BASE     0x0C400000UL
#define MT_GIC_IS_V3        1

/* --- Подсистема дисплея (снято с устройства) ---
 * Цепочка вывода: OVL -> RDMA -> COLOR -> CCORR -> AAL -> GAMMA -> DITHER -> DSI */
#define MT_DISP_OVL0_BASE   0x1400B000UL    /* исправлено: не 0x14008000 */
#define MT_DISP_OVL0_2L     0x1400C000UL
#define MT_DISP_RDMA0_BASE  0x1400D000UL
#define MT_DISP_MUTEX0_BASE 0x14001000UL
#define MT_DISP_PWM_BASE    0x1100E000UL    /* яркость подсветки        */

/* Смещения регистров DISP_OVL (одинаковы во всём семействе MTK,
 * см. drivers/gpu/drm/mediatek/mtk_disp_ovl.c) */
#define OVL_L0_SRC_SIZE     0x0038          /* [31:16]=высота, [15:0]=ширина */
#define OVL_L0_PITCH        0x0044          /* байт в строке                 */
#define OVL_L0_ADDR         0x0F40          /* физический адрес буфера       */

/* --- I2C: девять контроллеров. Тачскрин сидит на одном из них,
 * определим на этапе 5 по узлу touch в DTB. --- */
#define MT_I2C0_BASE        0x11007000UL
#define MT_I2C1_BASE        0x11008000UL
#define MT_I2C2_BASE        0x11009000UL
#define MT_I2C3_BASE        0x1100F000UL
#define MT_I2C4_BASE        0x11011000UL
#define MT_I2C5_BASE        0x11016000UL
#define MT_I2C6_BASE        0x1100D000UL

#define MT_USB0_BASE        0x11200000UL

/* --- Экран merlinnfc --- */
#define MERLIN_FB_WIDTH     1080
#define MERLIN_FB_HEIGHT    2340
#define MERLIN_FB_BPP       32

/* PPI таймеров. Используем виртуальный: физический из EL1 доступен только
 * с разрешения EL2, а EL2 здесь чужой. Подробности в include/timer.h. */
#define MT_ARCH_TIMER_VIRT  27
#define MT_ARCH_TIMER_IRQ   30              /* non-secure physical timer PPI */

#endif
