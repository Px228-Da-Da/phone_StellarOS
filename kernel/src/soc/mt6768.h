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

/* --- DRAM ---
 * База 0x40000000 подтверждена: именно туда LK кладёт ядро (base + 0x80000).
 *
 * Объём: у этого экземпляра 4 ГБ — Android показывает MemTotal 3 790 276 КБ,
 * остальное съедают закрытые области. Это подтверждает верхнюю границу
 * отображения, но НЕ означает, что все 4 ГБ наши.
 *
 * TODO-VERIFY: где именно лежит доступная нам память. В /reserved-memory
 * устройства двадцать один блок: ATF, TEE, модем, фреймбуфер, pstore.
 * Их физические адреса лежат в свойствах reg, а те без root не читаются —
 * возьмём их из DTB, который LK передаёт нашему же ядру (этап 5).
 * До тех пор аллокатору отдаётся 2 ГБ, и прошивать нельзя: выдать чужую
 * защищённую область под кучу — это не ошибка чтения, это нарушение
 * защиты EMI и мгновенная перезагрузка.
 */
#define MT_RAM_BASE         0x40000000UL
#define MT_RAM_SIZE         (4096UL * 1024 * 1024)   /* сколько отображаем */
#define MT_RAM_USABLE       (2048UL * 1024 * 1024)   /* чем распоряжаемся  */

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

/* --- Ядра ---
 * СНЯТО С УСТРОЙСТВА (device-info/dt-tree.txt, /proc/device-tree/cpus):
 *   cpu@000 cpu@001 cpu@002 cpu@003   — кластер 0, MPIDR 0x000..0x003
 *   cpu@100 cpu@101 cpu@102 cpu@103   — кластер 1, MPIDR 0x100..0x103
 *
 * То есть аффинити НЕ плоское: восемь ядер разложены по двум кластерам
 * по четыре. Именно поэтому загрузочное ядро отбирается по Aff0..Aff2
 * целиком: по одному лишь Aff0 ядро 0x100 сошло бы за загрузочное.
 *
 * Отдельно стоит cpu-map: там cluster0 из шести ядер и cluster1 из двух —
 * это группировка по типу ядер (6xA55 + 2xA75) для планировщика Linux,
 * и с аффинити MPIDR она не совпадает. Нам нужна вторая, MPIDR.
 */
#define MT_CPU_CLUSTERS     2
#define MT_CPUS_PER_CLUSTER 4

/* --- Контроллер прерываний ---
 * Узел называется gic500@0c000000 => это GIC-500, то есть GICv3.
 * ВАЖНО для этапа 3: GICv3 настраивается через системные регистры
 * ICC_* (msr/mrs), а не через MMIO CPU-интерфейс как GICv2.
 * gic_cpu@0c400000 — блок GIC CPU wakeup, не путать с GICC из v2. */
#define MT_GICD_BASE        0x0C000000UL    /* distributor             */
/* Обе базы теперь берутся из device tree на старте (см. gic_bases_from_fdt):
 * ядро ищет узел с compatible = "arm,gic-v3" и читает его reg. Значения ниже
 * остались запасным вариантом на случай, если дерева не окажется вовсе.
 * Адрес редистрибьюторов — по раскладке GIC-500 в ядрах MediaTek mt6765/mt6768;
 * если он неверен, ядро скажет об этом строкой «GICR ИЗ DTB ... В КАРТЕ БЫЛО». */
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

/* --- Выводы и прерывания от них ---
 * СНЯТО С УСТРОЙСТВА: gpio@10005000, io_cfg_*@10002000.., apirq@1000b000.
 * Выводов 186, pinctrl = mediatek,mt6768-pinctrl.
 *
 * Три разных блока, и путать их нельзя:
 *   GPIO     направление, чтение и запись состояния вывода
 *   IO_CFG   подтяжки и сила тока, восемь банков по 0x200
 *   EINT     прерывания от выводов; именно сюда приходит сигнал тачскрина
 */
#define MT_GPIO_BASE        0x10005000UL
#define MT_IOCFG_BASE       0x10002000UL    /* 8 банков по 0x200 */
#define MT_IOCFG_BANKS      8
#define MT_EINT_BASE        0x1000B000UL
#define MT_GPIO_COUNT       186

/* --- SPI ---
 * Тачскрин сидит здесь: spi0@1100a000, узел novatek@0, cs 0, 8 МГц.
 * Прерывание контроллера: SPI 0x8a => INTID 138 + 32 = 170. */
#define MT_SPI0_BASE        0x1100A000UL
#define MT_SPI2_BASE        0x11012000UL    /* сканер отпечатка */
#define MT_SPI3_BASE        0x11013000UL    /* усилитель динамика cs35l41 */
#define MT_SPI5_BASE        0x11015000UL

/* Тачскрин Novatek NT36xxx (параметры из раздела dtbo официального ROM) */
#define NVT_SPI_BASE        MT_SPI0_BASE
#define NVT_SPI_CS          0
/* Тачскрин выдерживает 8 МГц, и вендорный драйвер работает на них. У нас
 * же на шести с половиной поток приходил с плавающим первым байтом, а
 * контрольная сумма залитого не сходилась. Запас по времени важнее
 * скорости: заливка прошивки идёт один раз при включении. */
#define NVT_SPI_HZ          1000000
#define NVT_GPIO_RESET      92
#define NVT_GPIO_IRQ        1

/* --- Сторожевой таймер (TOPRGU) ---
 * СНЯТО С УСТРОЙСТВА: узел toprgu@10007000 в device tree.
 *
 * Его запускает preloader, и дальше кто-то обязан регулярно его гладить.
 * Linux делает это драйвером mtk_wdt; наше ядро о нём не знает вообще,
 * поэтому железо перезагружает телефон само через несколько секунд —
 * независимо от того, работает наш код или нет. Именно так выглядела
 * первая прошивка: чёрный экран и перезагрузка по кругу.
 *
 * Выключаем его первым же действием в kmain. Заодно это возвращает нам
 * единственный доступный канал отладки: без вывода мы можем различать
 * «зависли» и «упали» только по тому, перезагрузился телефон или нет,
 * а с живым сторожем он перезагружается всегда.
 *
 * Запись без ключа в старших битах регистр игнорирует — защита от
 * случайной записи (drivers/watchdog/mtk_wdt.c). */
#define MT_TOPRGU_BASE      0x10007000UL
#define WDT_MODE            0x00
#define WDT_MODE_KEY        0x22000000U     /* без него запись не примут */
#define WDT_MODE_EN         (1U << 0)       /* сам сторож               */
#define WDT_MODE_IRQ        (1U << 3)
#define WDT_MODE_DUAL       (1U << 6)
#define WDT_RESTART         0x08
#define WDT_RESTART_KEY     0x1971          /* «погладить», если решим держать */
#define WDT_SWRST           0x14            /* немедленный сброс по команде   */
#define WDT_SWRST_KEY       0x1209

/* --- RDMA: синхронизация с выводом кадра ---
 * DISP_RDMA0 читает буфер из памяти и отдаёт его дальше по цепочке к панели,
 * поэтому именно он знает, когда кадр закончился. Смещения и биты — из
 * drivers/gpu/drm/mediatek/mtk_disp_rdma.c, они общие для семейства MTK.
 *
 * Зачем: подмена адреса буфера в произвольный момент даёт шов поперёк
 * экрана — верх из старого кадра, низ из нового. Ждём конца вывода и только
 * тогда переключаем. Прерывание для этого не нужно, флаг можно опрашивать. */
#define RDMA_INT_ENABLE     0x00
#define RDMA_INT_STATUS     0x04
#define RDMA_INT_REG_UPDATE (1U << 0)
#define RDMA_INT_FRAME_START (1U << 1)
#define RDMA_INT_FRAME_END  (1U << 2)

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
