#ifndef SOC_QEMU_VIRT_H
#define SOC_QEMU_VIRT_H
/*
 * Карта регистров QEMU -M virt. Наша «песочница»: сюда сначала
 * приезжает любая новая фича, и только потом на телефон.
 */

/* DRAM. Размер должен совпадать с -m в строке запуска QEMU (см. Makefile) */
#define QEMU_RAM_BASE       0x40000000UL
#define QEMU_RAM_SIZE       (1024UL * 1024 * 1024)

#define QEMU_UART0_BASE     0x09000000UL    /* PL011                     */

/* Контроллер прерываний.
 * ВАЖНО: запускать QEMU нужно с -M virt,gic-version=3 (см. Makefile).
 * По умолчанию virt даёт GICv2, а в merlin стоит GIC-500, то есть v3.
 * Держим в эмуляторе ту же версию, что и на телефоне: иначе отлаживали бы
 * код, который на железо всё равно не поедет. */
#define QEMU_GICD_BASE      0x08000000UL    /* distributor               */
#define QEMU_GICR_BASE      0x080A0000UL    /* redistributors, шаг 128 КБ */
#define QEMU_GICC_BASE      0x08010000UL    /* CPU-интерфейс GICv2, не нужен */

/* PL011 */
#define PL011_DR            0x00
#define PL011_FR            0x18
#define PL011_FR_TXFF       (1 << 5)        /* tx FIFO полон             */
#define PL011_FR_RXFE       (1 << 4)        /* rx FIFO пуст              */

#endif
