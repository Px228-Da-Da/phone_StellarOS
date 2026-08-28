/*
 * string.c — работа с памятью.
 *
 * Написано просто и побайтово. Оптимизировать по 8 байт за раз смысла пока нет:
 * с включёнными кэшами узкое место не здесь, а любая хитрость в копировании —
 * это лишний способ отладки на телефоне без отладчика.
 */
#include "string.h"

void *memset(void *dst, int c, size_t n)
{
    u8 *p = (u8 *)dst;

    while (n--)
        *p++ = (u8)c;

    return dst;
}

void *memcpy(void *dst, const void *src, size_t n)
{
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;

    while (n--)
        *d++ = *s++;

    return dst;
}

/* В отличие от memcpy обязан работать при перекрытии областей:
 * если приёмник правее источника, копируем с конца. */
void *memmove(void *dst, const void *src, size_t n)
{
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;

    if (d == s || n == 0)
        return dst;

    if (d < s) {
        while (n--)
            *d++ = *s++;
    } else {
        d += n;
        s += n;
        while (n--)
            *--d = *--s;
    }

    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const u8 *x = (const u8 *)a;
    const u8 *y = (const u8 *)b;

    while (n--) {
        if (*x != *y)
            return (int)*x - (int)*y;
        x++;
        y++;
    }

    return 0;
}
