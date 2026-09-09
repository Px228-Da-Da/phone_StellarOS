/*
 * Обои.
 *
 * Экран у нас 1080x2340, то есть десять мегабайт на кадр. Вшить картинку
 * такого размера в образ можно — загрузочный раздел 64 МБ, — но незачем:
 * обои это плавные переходы, и в них нет подробностей, ради которых
 * стоило бы возить каждую точку.
 *
 * Поэтому в образе лежит половинного размера, 540x1170, а полный кадр
 * собирается один раз при загрузке увеличением ровно вдвое. Вдвое — не
 * случайное число: 540*2 = 1080 и 1170*2 = 2340 в точности, поэтому
 * дробного масштаба не возникает вовсе, и каждая новая точка получается
 * из четырёх соседних старых простым взвешиванием.
 *
 * Считаем один раз и держим готовый кадр в памяти. Пересчитывать его на
 * каждый показ значило бы платить взвешиванием за два с половиной
 * миллиона точек шестьдесят раз в секунду — на это нет ни времени, ни
 * смысла: картинка не меняется.
 *
 * Память берём страницами у PMM, а не у кучи: куча у нас двенадцать
 * килобайт и заведена для мелочей.
 */
#include "wall.h"
#include "pmm.h"
#include "print.h"
#include "string.h"
#include "fb.h"

#if defined(BOARD_MERLIN)

#include "wall_img.h"           /* wall_img[], собран из PNG при сборке */

static u32 *full;               /* готовый кадр во весь экран */
static u32 full_w, full_h;

static u32 sti_u32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

/*
 * Развернуть .sti в готовые точки.
 *
 * Формат тот же, что у значков оболочки: подпись, ширина, высота,
 * признак сжатия повторами и длина. Повтор — байт длины и сама точка.
 */
static u32 *sti_unpack(const u8 *sti, u32 *w_out, u32 *h_out)
{
    u32 w, h, flags, len, n;
    const u8 *p;
    u32 *out;

    if (sti[0] != 'S' || sti[1] != 'T' || sti[2] != 'I' || sti[3] != '1')
        return 0;

    w = sti_u32(sti + 4);
    h = sti_u32(sti + 8);
    flags = sti_u32(sti + 12);
    len = sti_u32(sti + 16);
    p = sti + 20;
    n = w * h;

    out = pmm_alloc_pages((u32)((n * 4 + PAGE_SIZE - 1) / PAGE_SIZE));
    if (!out)
        return 0;

    if (!(flags & 1)) {
        memcpy(out, p, n * 4);
    } else {
        u32 i = 0;

        for (u32 off = 0; off + 5 <= len && i < n; off += 5) {
            u32 run = p[off];
            u32 px = sti_u32(p + off + 1);

            while (run-- && i < n)
                out[i++] = px;
        }
    }

    *w_out = w;
    *h_out = h;
    return out;
}

/*
 * Увеличить вдвое, взвешивая соседей.
 *
 * Простое размножение точек оставило бы на плавных переходах квадраты
 * два на два — на градиенте это видно сразу, полосами. Поэтому каждая
 * новая точка берётся как взвешенная смесь четырёх соседних старых.
 *
 * Веса при увеличении ровно вдвое получаются целыми: центр новой точки
 * попадает либо на четверть, либо на три четверти между старыми, то есть
 * коэффициенты 3 и 1 из четырёх. Дробей не нужно, всё считается в целых.
 */
static u32 mix4(u32 a, u32 b, u32 c, u32 d, u32 wa, u32 wb, u32 wc, u32 wd)
{
    u32 sum = wa + wb + wc + wd;
    u32 r = ((a >> 16 & 0xFF) * wa + (b >> 16 & 0xFF) * wb
           + (c >> 16 & 0xFF) * wc + (d >> 16 & 0xFF) * wd) / sum;
    u32 g = ((a >>  8 & 0xFF) * wa + (b >>  8 & 0xFF) * wb
           + (c >>  8 & 0xFF) * wc + (d >>  8 & 0xFF) * wd) / sum;
    u32 bl = ((a      & 0xFF) * wa + (b      & 0xFF) * wb
           + (c      & 0xFF) * wc + (d      & 0xFF) * wd) / sum;

    return 0xFF000000u | (r << 16) | (g << 8) | bl;
}

static void upscale2(const u32 *src, u32 sw, u32 sh)
{
    for (u32 y = 0; y < full_h; y++) {
        /*
         * Целая часть — какие две строки источника смешивать, дробная —
         * в какой из двух половин мы находимся. При увеличении вдвое
         * дробная часть бывает только двух видов, отсюда веса 3:1 и 1:3.
         */
        u32 sy = y >> 1;
        u32 sy2 = (sy + 1 < sh) ? sy + 1 : sy;
        u32 wy = (y & 1) ? 1u : 3u;     /* вес ДАЛЬНЕЙ строки */
        u32 *drow = full + (u64)y * full_w;
        const u32 *r0 = src + (u64)sy * sw;
        const u32 *r1 = src + (u64)sy2 * sw;

        for (u32 x = 0; x < full_w; x++) {
            u32 sx = x >> 1;
            u32 sx2 = (sx + 1 < sw) ? sx + 1 : sx;
            u32 wx = (x & 1) ? 1u : 3u;

            drow[x] = mix4(r0[sx], r0[sx2], r1[sx], r1[sx2],
                           (4 - wy) * (4 - wx), (4 - wy) * wx,
                           wy * (4 - wx), wy * wx);
        }
    }
}

/*
 * Затемнить верх.
 *
 * У этих обоев верхняя треть светлая, а поверх неё идут белые часы и
 * строка состояния — без затемнения они там тонут. Телефон делает ровно
 * это же: под верхом лежит едва заметная тень, и заметна она только тем,
 * что текст читается.
 *
 * Впекаем в готовый кадр ОДИН РАЗ. Накладывать её на каждый кадр значило
 * бы читать и переписывать семьсот тысяч точек шестьдесят раз в секунду
 * ради того, что никогда не меняется.
 *
 * Сила падает по квадрату, а не прямо: линейная тень видна полосой там,
 * где кончается, а квадрат сходит на нет незаметно.
 */
#define SCRIM_MAX   110         /* из 255, в самом верху */

static void scrim(void)
{
    u32 depth = full_h / 3;

    for (u32 y = 0; y < depth; y++) {
        u32 left = depth - y;
        u32 a = (u32)((u64)SCRIM_MAX * left * left / ((u64)depth * depth));
        u32 *row = full + (u64)y * full_w;

        if (!a)
            continue;
        for (u32 x = 0; x < full_w; x++) {
            u32 p = row[x];
            u32 r = (p >> 16 & 0xFF) * (255 - a) / 255;
            u32 g = (p >>  8 & 0xFF) * (255 - a) / 255;
            u32 b = (p       & 0xFF) * (255 - a) / 255;

            row[x] = 0xFF000000u | (r << 16) | (g << 8) | b;
        }
    }
}

void wall_init(void)
{
    u32 sw = 0, sh = 0;
    u32 *small;
    u32 pages;

    if (full)
        return;

    {
        u64 base;
        u32 stride;

        fb_info(&base, &full_w, &full_h, &stride);
    }
    if (!full_w || !full_h)
        return;

    small = sti_unpack(wall_img, &sw, &sh);
    if (!small) {
        kprintf("ОБОИ     : НЕ РАЗОБРАЛ КАРТИНКУ\n");
        return;
    }

    /*
     * Увеличение вдвое — единственное, что мы умеем, и оно же
     * единственное, что здесь нужно. Если картинка вдруг не той
     * половины, честнее отказаться, чем растянуть её криво.
     */
    if (sw * 2 != full_w || sh * 2 != full_h) {
        kprintf("ОБОИ     : %ux%u НЕ ПОЛОВИНА ОТ %ux%u, НЕ БЕРУ\n",
                sw, sh, full_w, full_h);
        pmm_free_pages(small, (u32)((sw * sh * 4 + PAGE_SIZE - 1) / PAGE_SIZE));
        return;
    }

    pages = (u32)(((u64)full_w * full_h * 4 + PAGE_SIZE - 1) / PAGE_SIZE);
    full = pmm_alloc_pages(pages);
    if (!full) {
        kprintf("ОБОИ     : НЕ ХВАТИЛО %u СТРАНИЦ\n", pages);
        pmm_free_pages(small, (u32)((sw * sh * 4 + PAGE_SIZE - 1) / PAGE_SIZE));
        return;
    }

    upscale2(small, sw, sh);
    pmm_free_pages(small, (u32)((sw * sh * 4 + PAGE_SIZE - 1) / PAGE_SIZE));
    scrim();

    kprintf("ОБОИ     : %ux%u -> %ux%u, %u КБ\n",
            sw, sh, full_w, full_h, pages * 4);
}

const u32 *wall_pixels(u32 *w, u32 *h)
{
    if (!full)
        return 0;
    if (w)
        *w = full_w;
    if (h)
        *h = full_h;
    return full;
}

#else

void wall_init(void) { }
const u32 *wall_pixels(u32 *w, u32 *h) { (void)w; (void)h; return 0; }

#endif
