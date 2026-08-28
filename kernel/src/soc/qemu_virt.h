#ifndef SOC_QEMU_VIRT_H
#define SOC_QEMU_VIRT_H
/*
 * Карта регистров QEMU -M virt. Наша «песочница»: сюда сначала
 * приезжает любая новая фича, и только потом на телефон.
 */

#define QEMU_UART0_BASE     0x09000000UL    /* PL011                     */
#define QEMU_GICD_BASE      0x08000000UL
#define QEMU_GICC_BASE      0x08010000UL
#define QEMU_ARCH_TIMER_IRQ 30

/* PL011 */
#define PL011_DR            0x00
#define PL011_FR            0x18
#define PL011_FR_TXFF       (1 << 5)        /* tx FIFO полон             */
#define PL011_FR_RXFE       (1 << 4)        /* rx FIFO пуст              */

#endif
