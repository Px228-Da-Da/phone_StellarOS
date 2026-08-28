#ifndef PRINT_H
#define PRINT_H
#include "types.h"

/*
 * Мини-printf ядра. Пишет одновременно в UART и на экран,
 * поэтому один и тот же код отлаживается и в QEMU, и на телефоне.
 * Поддержка: %s %c %d %u %x %p %% и модификатор l (%lx, %lu, %ld).
 */
void kprintf(const char *fmt, ...);
void kputs(const char *s);

#endif
