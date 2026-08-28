#ifndef UART_H
#define UART_H
#include "types.h"

void uart_init(void);
void uart_putc(char c);
void uart_puts(const char *s);
int  uart_getc_nb(void);   /* -1 если нет данных */

#endif
