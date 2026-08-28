#ifndef STRING_H
#define STRING_H
#include "types.h"

/*
 * Минимальный набор работы с памятью.
 *
 * Эти четыре функции обязаны существовать даже в -ffreestanding: компилятор
 * имеет право сам вставить вызов memset или memcpy там, где мы их не писали —
 * например, при инициализации структуры. Без них ядро не слинкуется, причём
 * ошибка вылезет не там, где её ожидаешь.
 */
void *memset(void *dst, int c, size_t n);
void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
int   memcmp(const void *a, const void *b, size_t n);

#endif
