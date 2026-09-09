#ifndef WALL_H
#define WALL_H
#include "types.h"

/*
 * Обои: готовый кадр во весь экран, собранный один раз при загрузке.
 */

/* Разобрать и увеличить. Печатает, что получилось. */
void wall_init(void);

/* Готовые точки и размер, или 0 — обоев нет. */
const u32 *wall_pixels(u32 *w, u32 *h);

#endif
