/*
 * Драйвер последовательного порта.
 *
 * BOARD_QEMU  — PL011, работает сразу, наш основной канал отладки.
 * BOARD_MERLIN — 8250-совместимый MTK UART. Физически до него нужно
 *   добраться пайкой к тест-пойнтам; пока провода нет, вызовы просто
 *   уходят в никуда и ничему не мешают. Настоящий вывод на телефоне —
 *   через фреймбуфер (fb.c).
 */
#include "uart.h"
#include "io.h"

#if defined(BOARD_QEMU)
#include "soc/qemu_virt.h"
#define UART_BASE QEMU_UART0_BASE

void uart_init(void)
{
    /* QEMU отдаёт PL011 уже настроенным загрузчиком — трогать нечего */
}

void uart_putc(char c)
{
    if (c == '\n')
        uart_putc('\r');
    while (mmio_read32(UART_BASE + PL011_FR) & PL011_FR_TXFF)
        ;
    mmio_write32(UART_BASE + PL011_DR, (u32)c);
}

int uart_getc_nb(void)
{
    if (mmio_read32(UART_BASE + PL011_FR) & PL011_FR_RXFE)
        return -1;
    return (int)(mmio_read32(UART_BASE + PL011_DR) & 0xFF);
}

#elif defined(BOARD_MERLIN)
#include "soc/mt6768.h"
#define UART_BASE MT_UART0_BASE

void uart_init(void)
{
    /* Скорость и линию уже настроил preloader/LK (921600 8N1).
     * Переинициализация без знания делителя тактовой только всё сломает,
     * поэтому пользуемся тем, что оставили. */
}

void uart_putc(char c)
{
    int guard = 100000;                     /* не виснуть, если UART мёртв */

    if (c == '\n')
        uart_putc('\r');
    while (!(mmio_read32(UART_BASE + UART_LSR) & UART_LSR_THRE) && --guard)
        ;
    mmio_write32(UART_BASE + UART_RBR_THR, (u32)(u8)c);
}

int uart_getc_nb(void)
{
    if (!(mmio_read32(UART_BASE + UART_LSR) & UART_LSR_DR))
        return -1;
    return (int)(mmio_read32(UART_BASE + UART_RBR_THR) & 0xFF);
}

#else
#error "Не задана плата: определите BOARD_QEMU или BOARD_MERLIN"
#endif

void uart_puts(const char *s)
{
    while (*s)
        uart_putc(*s++);
}
