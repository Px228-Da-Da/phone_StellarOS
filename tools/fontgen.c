/*
 * fontgen — превратить TrueType в растровый шрифт StellarOS (.stf).
 *
 * Ядру шрифт нужен готовым: разбирать кривые Безье во время загрузки не
 * на чем — ни чисел с плавающей точкой, ни памяти под них там нет, да и
 * незачем: набор знаков известен заранее и не меняется от запуска к
 * запуску. Поэтому вся работа делается здесь, на компьютере, а в образ
 * едут пиксели.
 *
 * Растеризатор свой, а не freetype: в системе его нет, ставить пакеты на
 * чужую машину ради сборки — плохая привычка, а формат TrueType описан
 * спецификацией и разбирается однозначно. Заодно шрифтовой путь
 * становится нашим целиком, как компилятор и виртуальная машина.
 *
 * Что делает:
 *   1. читает таблицы head, maxp, hhea, hmtx, cmap, loca, glyf
 *   2. по cmap находит глиф для каждого нужного знака
 *   3. разворачивает контуры (в TrueType это квадратичные кривые) в
 *      ломаные
 *   4. заливает их по правилу ненулевого поворота со сглаживанием —
 *      шестнадцать проб на пиксель
 *   5. пишет .stf: заголовок, коды, глифы, пиксели
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* Байт на знак в файле шрифта. Шестнадцать, а не восемь: на кегле 190
 * подъём цифры равен -137, и в знаковый байт он не помещается. */
#define GLYPH_REC   16

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;
typedef short          s16;

static u8   *ttf;
static long  ttf_len;

/* Все числа в TrueType — старшим байтом вперёд, в отличие от нашей машины */
static u32 rd32(long o)
{
    if (o < 0 || o + 4 > ttf_len)
        return 0;
    return ((u32)ttf[o] << 24) | ((u32)ttf[o + 1] << 16) |
           ((u32)ttf[o + 2] << 8) | ttf[o + 3];
}

static u16 rd16(long o)
{
    if (o < 0 || o + 2 > ttf_len)
        return 0;
    return (u16)((ttf[o] << 8) | ttf[o + 1]);
}

static s16 rds16(long o) { return (s16)rd16(o); }

static long table(const char *tag, u32 *len)
{
    u16 n = rd16(4);

    for (u16 i = 0; i < n; i++) {
        long rec = 12 + (long)i * 16;

        /* запись каталога: метка, контрольная сумма, смещение, длина */
        if (memcmp(ttf + rec, tag, 4) == 0) {
            if (len)
                *len = rd32(rec + 12);
            return (long)rd32(rec + 8);
        }
    }

    return -1;
}

/* --- Разбор cmap ---------------------------------------------------- */

static long cmap_sub;       /* выбранная подтаблица */
static int  cmap_format;

/*
 * Ищем подтаблицу, понимающую Юникод. Порядок предпочтений важен: формат
 * 12 покрывает знаки за пределами основной плоскости — там живут эмодзи,
 * а формат 4 их не достаёт вовсе.
 */
static void cmap_pick(void)
{
    long cm = table("cmap", NULL);
    u16 n;
    long best = -1;
    int best_fmt = -1;

    if (cm < 0) {
        fprintf(stderr, "fontgen: в шрифте нет cmap\n");
        exit(1);
    }
    n = rd16(cm + 2);

    for (u16 i = 0; i < n; i++) {
        long rec = cm + 4 + (long)i * 8;
        u16 plat = rd16(rec), enc = rd16(rec + 2);
        long sub = cm + (long)rd32(rec + 4);
        int fmt = rd16(sub);
        int good = (plat == 3 && (enc == 1 || enc == 10)) || plat == 0;

        if (!good || (fmt != 4 && fmt != 12))
            continue;
        if (best < 0 || (fmt == 12 && best_fmt != 12)) {
            best = sub;
            best_fmt = fmt;
        }
    }

    if (best < 0) {
        fprintf(stderr, "fontgen: в cmap нет понятной подтаблицы\n");
        exit(1);
    }
    cmap_sub = best;
    cmap_format = best_fmt;
}

static u32 glyph_of(u32 cp)
{
    if (cmap_format == 12) {
        u32 groups = rd32(cmap_sub + 12);

        for (u32 i = 0; i < groups; i++) {
            long g = cmap_sub + 16 + (long)i * 12;
            u32 a = rd32(g), b = rd32(g + 4);

            if (cp >= a && cp <= b)
                return rd32(g + 8) + (cp - a);
        }
        return 0;
    }

    {   /* формат 4 */
        u16 segs = rd16(cmap_sub + 6) / 2;
        long ends = cmap_sub + 14;
        long starts = ends + (long)segs * 2 + 2;
        long deltas = starts + (long)segs * 2;
        long ranges = deltas + (long)segs * 2;

        if (cp > 0xFFFF)
            return 0;

        for (u16 i = 0; i < segs; i++) {
            u16 end = rd16(ends + i * 2);
            u16 start, ro;
            s16 delta;
            u32 g;

            if (cp > end)
                continue;
            start = rd16(starts + i * 2);
            if (cp < start)
                return 0;
            delta = rds16(deltas + i * 2);
            ro = rd16(ranges + i * 2);
            if (ro == 0)
                return (cp + (u32)delta) & 0xFFFF;

            g = rd16(ranges + i * 2 + ro + (long)(cp - start) * 2);
            if (!g)
                return 0;
            return (g + (u32)delta) & 0xFFFF;
        }
    }

    return 0;
}

/* --- Контуры -------------------------------------------------------- */

#define MAX_EDGES 20000

struct edge { double x0, y0, x1, y1; };

static struct edge edges[MAX_EDGES];
static int nedges;

static double px_scale;     /* единиц шрифта -> пикселей */

static void add_edge(double x0, double y0, double x1, double y1)
{
    if (y0 == y1)
        return;                 /* горизонтальные ничего не пересекают */
    if (nedges >= MAX_EDGES)
        return;
    edges[nedges].x0 = x0;
    edges[nedges].y0 = y0;
    edges[nedges].x1 = x1;
    edges[nedges].y1 = y1;
    nedges++;
}

/*
 * Квадратичная кривая в ломаную.
 *
 * Число отрезков считаем от размера: у мелкого кегля дробить нечего, а у
 * крупного ломаная не должна быть видна.
 */
static void add_quad(double x0, double y0, double cx, double cy,
                     double x1, double y1)
{
    double d = fabs(x0 - x1) + fabs(y0 - y1) + fabs(cx - x0) + fabs(cy - y0);
    int n = (int)(d / 3.0);
    double px = x0, py = y0;

    if (n < 2)
        n = 2;
    if (n > 24)
        n = 24;

    for (int i = 1; i <= n; i++) {
        double t = (double)i / n, u = 1 - t;
        double x = u * u * x0 + 2 * u * t * cx + t * t * x1;
        double y = u * u * y0 + 2 * u * t * cy + t * t * y1;

        add_edge(px, py, x, y);
        px = x;
        py = y;
    }
}

static long loca_off, glyf_off;
static int  loca_long;

static long glyph_data(u32 gid, u32 *size)
{
    long a, b;

    if (loca_long) {
        a = rd32(loca_off + (long)gid * 4);
        b = rd32(loca_off + (long)gid * 4 + 4);
    } else {
        a = rd16(loca_off + (long)gid * 2) * 2;
        b = rd16(loca_off + (long)gid * 2 + 2) * 2;
    }
    if (b <= a) {
        *size = 0;
        return -1;              /* пустой глиф — например, пробел */
    }
    *size = (u32)(b - a);
    return glyf_off + a;
}

/*
 * Развернуть глиф в отрезки. Смещение и масштаб — ради составных глифов:
 * Й в TrueType это И плюс краткая, и вторая часть приходит со своим
 * сдвигом.
 */
static void outline(u32 gid, double dx, double dy, double sx, double sy,
                    int depth)
{
    u32 size;
    long g = glyph_data(gid, &size);
    int ncont;

    if (g < 0 || depth > 4)
        return;

    ncont = rds16(g);

    if (ncont < 0) {            /* составной */
        long p = g + 10;

        for (;;) {
            u16 flags = rd16(p), idx = rd16(p + 2);
            double ox, oy, csx = 1, csy = 1;

            p += 4;
            if (flags & 0x0001) {           /* аргументы словами */
                ox = rds16(p);
                oy = rds16(p + 2);
                p += 4;
            } else {
                ox = (signed char)ttf[p];
                oy = (signed char)ttf[p + 1];
                p += 2;
            }
            if (!(flags & 0x0002)) {        /* не смещение, а номера точек */
                ox = 0;
                oy = 0;
            }
            if (flags & 0x0008) {
                csx = csy = rds16(p) / 16384.0;
                p += 2;
            } else if (flags & 0x0040) {
                csx = rds16(p) / 16384.0;
                csy = rds16(p + 2) / 16384.0;
                p += 4;
            } else if (flags & 0x0080) {
                csx = rds16(p) / 16384.0;
                csy = rds16(p + 6) / 16384.0;
                p += 8;
            }

            outline(idx, dx + ox * sx, dy + oy * sy, sx * csx, sy * csy,
                    depth + 1);

            if (!(flags & 0x0020))
                break;
        }
        return;
    }

    {
        long p = g + 10;
        int npts, i;
        u16 *ends = malloc((size_t)ncont * sizeof(u16));
        u8 *flags;
        double *xs, *ys;
        long q;
        u16 ilen;

        for (i = 0; i < ncont; i++)
            ends[i] = rd16(p + (long)i * 2);
        npts = ncont ? ends[ncont - 1] + 1 : 0;
        p += (long)ncont * 2;
        ilen = rd16(p);
        p += 2 + ilen;

        flags = malloc((size_t)npts);
        xs = malloc((size_t)npts * sizeof(double));
        ys = malloc((size_t)npts * sizeof(double));

        for (i = 0; i < npts; ) {
            u8 f = ttf[p++];

            flags[i++] = f;
            if (f & 0x08) {
                u8 rep = ttf[p++];

                while (rep-- && i < npts)
                    flags[i++] = f;
            }
        }

        q = p;
        {
            int v = 0;

            for (i = 0; i < npts; i++) {
                u8 f = flags[i];

                if (f & 0x02) {
                    int d = ttf[q++];

                    v += (f & 0x10) ? d : -d;
                } else if (!(f & 0x10)) {
                    v += rds16(q);
                    q += 2;
                }
                xs[i] = v;
            }
            v = 0;
            for (i = 0; i < npts; i++) {
                u8 f = flags[i];

                if (f & 0x04) {
                    int d = ttf[q++];

                    v += (f & 0x20) ? d : -d;
                } else if (!(f & 0x20)) {
                    v += rds16(q);
                    q += 2;
                }
                ys[i] = v;
            }
        }

        /* в пиксели: y в шрифте растёт вверх, у нас вниз */
        for (i = 0; i < npts; i++) {
            xs[i] = (xs[i] * sx + dx) * px_scale;
            ys[i] = -((ys[i] * sy + dy) * px_scale);
        }

        {
            int start = 0;

            for (i = 0; i < ncont; i++) {
                int end = ends[i], n = end - start + 1;
                double cx = 0, cy = 0, fx, fy, prx, pry;
                int have_ctrl = 0, j, k;

                if (n <= 0) {
                    start = end + 1;
                    continue;
                }

                /*
                 * Контур может начинаться с точки НЕ на кривой — тогда
                 * началом берём середину между ней и последней точкой:
                 * так предписывает формат, и без этого буквы с круглыми
                 * боками разъезжаются.
                 */
                if (flags[start] & 0x01) {
                    fx = xs[start];
                    fy = ys[start];
                    j = start + 1;
                } else if (flags[end] & 0x01) {
                    fx = xs[end];
                    fy = ys[end];
                    j = start;
                } else {
                    fx = (xs[start] + xs[end]) / 2;
                    fy = (ys[start] + ys[end]) / 2;
                    j = start;
                }
                prx = fx;
                pry = fy;

                for (k = 0; k < n; k++) {
                    int idx = start + ((j - start + k) % n);
                    double x = xs[idx], y = ys[idx];

                    if (flags[idx] & 0x01) {
                        if (have_ctrl) {
                            add_quad(prx, pry, cx, cy, x, y);
                            have_ctrl = 0;
                        } else {
                            add_edge(prx, pry, x, y);
                        }
                        prx = x;
                        pry = y;
                    } else {
                        if (have_ctrl) {
                            double mx = (cx + x) / 2, my = (cy + y) / 2;

                            add_quad(prx, pry, cx, cy, mx, my);
                            prx = mx;
                            pry = my;
                        }
                        cx = x;
                        cy = y;
                        have_ctrl = 1;
                    }
                }

                if (have_ctrl)
                    add_quad(prx, pry, cx, cy, fx, fy);
                else
                    add_edge(prx, pry, fx, fy);

                start = end + 1;
            }
        }

        free(ends);
        free(flags);
        free(xs);
        free(ys);
    }
}

/* --- Заливка -------------------------------------------------------- */

#define SUB 4                   /* проб на пиксель по каждой оси */

struct cross { double x; int dir; };

static int cmp_cross(const void *a, const void *b)
{
    double d = ((const struct cross *)a)->x - ((const struct cross *)b)->x;

    return (d < 0) ? -1 : (d > 0);
}

/*
 * Залить накопленные отрезки в 8-битную карту прозрачности.
 *
 * Правило ненулевого поворота, а не чётности: у букв с дырками (О, В, 8)
 * внутренний контур идёт в обратную сторону, и по чётности он
 * закрашивался бы наравне с внешним.
 */
static u8 *fill(int *out_x, int *out_y, int *out_w, int *out_h)
{
    double minx = 1e9, miny = 1e9, maxx = -1e9, maxy = -1e9;
    int x0, y0, w, h, i;
    u8 *cov;
    struct cross *cr;

    *out_w = *out_h = 0;
    *out_x = *out_y = 0;
    if (!nedges)
        return NULL;

    for (i = 0; i < nedges; i++) {
        double xs[2] = { edges[i].x0, edges[i].x1 };
        double ys[2] = { edges[i].y0, edges[i].y1 };

        for (int k = 0; k < 2; k++) {
            if (xs[k] < minx) minx = xs[k];
            if (xs[k] > maxx) maxx = xs[k];
            if (ys[k] < miny) miny = ys[k];
            if (ys[k] > maxy) maxy = ys[k];
        }
    }

    x0 = (int)floor(minx);
    y0 = (int)floor(miny);
    w = (int)ceil(maxx) - x0 + 1;
    h = (int)ceil(maxy) - y0 + 1;
    if (w <= 0 || h <= 0 || w > 255 || h > 255)
        return NULL;

    cov = calloc((size_t)w * h, 1);
    cr = malloc(sizeof(*cr) * (size_t)nedges);

    for (int py = 0; py < h; py++) {
        for (int s = 0; s < SUB; s++) {
            double y = y0 + py + (s + 0.5) / SUB;
            int n = 0, wind = 0;

            for (i = 0; i < nedges; i++) {
                double ya = edges[i].y0, yb = edges[i].y1;
                double lo = ya < yb ? ya : yb, hi = ya < yb ? yb : ya;

                if (y < lo || y >= hi)
                    continue;
                cr[n].x = edges[i].x0 + (y - ya) *
                          (edges[i].x1 - edges[i].x0) / (yb - ya);
                cr[n].dir = (yb > ya) ? 1 : -1;
                n++;
            }
            if (n < 2)
                continue;
            qsort(cr, (size_t)n, sizeof(*cr), cmp_cross);

            for (i = 0; i + 1 < n; i++) {
                double xa, xb;
                int px;

                wind += cr[i].dir;
                if (!wind)
                    continue;
                xa = cr[i].x;
                xb = cr[i + 1].x;

                for (px = 0; px < w; px++) {
                    for (int t = 0; t < SUB; t++) {
                        double x = x0 + px + (t + 0.5) / SUB;

                        if (x >= xa && x < xb)
                            cov[(size_t)py * w + px]++;
                    }
                }
            }
        }
    }

    for (i = 0; i < w * h; i++) {
        int v = cov[i] * 255 / (SUB * SUB);

        cov[i] = (u8)(v > 255 ? 255 : v);
    }

    free(cr);
    *out_x = x0;
    *out_y = y0;
    *out_w = w;
    *out_h = h;
    return cov;
}

/* --- Набор знаков --------------------------------------------------- */

#define MAX_CHARS 512

static u32 charset[MAX_CHARS];
static int ncs;

static void add_range(u32 a, u32 b)
{
    for (u32 c = a; c <= b && ncs < MAX_CHARS; c++)
        charset[ncs++] = c;
}

/*
 * Метрики знака держим широкими ВНУТРИ тоже, а не только в файле.
 *
 * Сначала я расширил поля в файле и решил, что дело сделано, — а числа
 * остались прежними: переполнение случалось здесь, при укладке в эту
 * структуру, задолго до записи. Байтовых полей хватало, пока начертания
 * были мелкими; на кегле 190 подъём цифры равен -137 и в знаковый байт
 * не помещается.
 */
struct glyph_out {
    u32 code;
    u32 off;
    u16 w, h, adv;
    s16 left, top;
    u8 *bits;
};

static struct glyph_out gl[MAX_CHARS];

int main(int argc, char **argv)
{
    FILE *f, *out;
    /* Десять, а не восемь: под макет понадобились ещё два кегля —
     * крупные часы и дата на экране блокировки. */
    int sizes[10], nsizes = 0;
    const char *face_chars[10];
    long head, hhea, hmtx;
    u16 units, nhm;
    double asc, desc;
    u32 blob_off;
    u32 faces[8][8];
    int nface = 0;

    if (argc < 4) {
        fprintf(stderr,
                "как пользоваться: fontgen шрифт.ttf 16,24,32,48 шрифт.stf\n");
        return 1;
    }

    f = fopen(argv[1], "rb");
    if (!f) {
        fprintf(stderr, "fontgen: не открыть %s\n", argv[1]);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    ttf_len = ftell(f);
    fseek(f, 0, SEEK_SET);
    ttf = malloc((size_t)ttf_len);
    if (fread(ttf, 1, (size_t)ttf_len, f) != (size_t)ttf_len) {
        fprintf(stderr, "fontgen: файл читается не целиком\n");
        return 1;
    }
    fclose(f);

    /*
     * Описание начертаний: "16,24,32,48,140:StellarOS".
     *
     * Двоеточие ограничивает набор знаков. Нужно ради логотипа: крупный
     * кегль со всеми знаками весит под мегабайт, а на заставке нужны
     * ровно буквы названия. Платить мегабайтом за то, что показывается
     * один раз при загрузке, — расточительство, которое потом не
     * вычистить.
     */
    {
        const char *p = argv[2];

        /* Предел тот же, что у массивов выше. Раньше здесь стояла
         * восьмёрка отдельным числом — и девятый кегль молча не
         * попадал в шрифт, а сборка при этом отчитывалась успехом. */
        while (*p && nsizes < (int)(sizeof(sizes) / sizeof(sizes[0]))) {
            sizes[nsizes] = atoi(p);
            face_chars[nsizes] = NULL;
            while (*p && *p != ',' && *p != ':')
                p++;
            if (*p == ':') {
                face_chars[nsizes] = ++p;
                while (*p && *p != ',')
                    p++;
            }
            nsizes++;
            if (*p == ',') {
                *(char *)p = 0;         /* обрезаем список знаков */
                p++;
            }
        }
    }

    head = table("head", NULL);
    hhea = table("hhea", NULL);
    hmtx = table("hmtx", NULL);
    loca_off = table("loca", NULL);
    glyf_off = table("glyf", NULL);
    if (head < 0 || loca_off < 0 || glyf_off < 0) {
        fprintf(stderr, "fontgen: это не TrueType с контурами\n");
        return 1;
    }

    units = rd16(head + 18);
    loca_long = rds16(head + 50);
    nhm = rd16(hhea + 34);
    asc = rds16(hhea + 4);
    desc = rds16(hhea + 6);
    cmap_pick();

    add_range(0x20, 0x7E);              /* латиница, цифры, знаки */
    add_range(0x0410, 0x044F);          /* А..я                   */
    charset[ncs++] = 0x0401;            /* Ё                      */
    charset[ncs++] = 0x0451;            /* ё                      */
    charset[ncs++] = 0x00AB;            /* «                      */
    charset[ncs++] = 0x00BB;            /* »                      */
    charset[ncs++] = 0x2014;            /* тире                   */
    charset[ncs++] = 0x2013;            /* короткое тире          */
    charset[ncs++] = 0x00B0;            /* градус                 */
    charset[ncs++] = 0x00B7;            /* точка посредине        */
    charset[ncs++] = 0x2116;            /* номер                  */
    charset[ncs++] = 0x2026;            /* многоточие             */

    out = fopen(argv[3], "wb");
    if (!out) {
        fprintf(stderr, "fontgen: не создать %s\n", argv[3]);
        return 1;
    }

    /*
     * Заголовок пишем дважды: сначала пустым, чтобы занять место, потом
     * поверх — с настоящими смещениями. Иначе пришлось бы держать все
     * начертания в памяти разом.
     */
    {
        u32 zero = 0;

        fwrite("STF2", 1, 4, out);
        fwrite(&zero, 4, 1, out);
        for (int i = 0; i < 8 * 8; i++)
            fwrite(&zero, 4, 1, out);
    }
    blob_off = 4 + 4 + 8 * 8 * 4;

    for (int si = 0; si < nsizes; si++) {
        int px = sizes[si];
        u32 total = 0;
        int used = 0;
        u32 own[64];
        const u32 *set = charset;
        int nset = ncs;

        px_scale = (double)px / units;

        if (face_chars[si]) {           /* только названные знаки */
            const unsigned char *q = (const unsigned char *)face_chars[si];
            int n = 0;

            while (*q && n < 64) {
                u32 cp = *q++;

                if (cp >= 0xC0 && cp <= 0xDF && (*q & 0xC0) == 0x80) {
                    cp = ((cp & 0x1F) << 6) | (*q & 0x3F);
                    q++;
                }
                own[n++] = cp;
            }
            /* по возрастанию: ядро ищет знак двоичным поиском */
            for (int a = 0; a < n; a++)
                for (int b = a + 1; b < n; b++)
                    if (own[b] < own[a]) {
                        u32 t = own[a];

                        own[a] = own[b];
                        own[b] = t;
                    }
            set = own;
            nset = n;
        }

        for (int i = 0; i < nset; i++) {
            u32 gid = glyph_of(set[i]);
            int gx, gy, gw, gh;
            u8 *bits;
            u32 adv;

            nedges = 0;
            if (gid)
                outline(gid, 0, 0, 1, 1, 0);
            bits = fill(&gx, &gy, &gw, &gh);

            adv = (gid < nhm) ? rd16(hmtx + (long)gid * 4)
                              : rd16(hmtx + (long)(nhm - 1) * 4);

            gl[used].code = set[i];
            gl[used].w = (u8)gw;
            gl[used].h = (u8)gh;
            /*
             * Приведения к signed char здесь и стояли — вот та самая
             * потеря. Правка формата и структуры без этой строки ничего
             * не давала: число обрезалось раньше, чем куда-либо попадало.
             *
             * Урок общий: расширяя поле, надо пройти ВЕСЬ путь значения,
             * а не только его конец. Я чинил этот путь трижды с конца и
             * трижды получал те же 119.
             */
            gl[used].left = (s16)gx;
            gl[used].top = (s16)gy;
            gl[used].adv = (u8)(adv * px_scale + 0.5);
            gl[used].bits = bits;
            gl[used].off = total;
            total += (u32)gw * gh;
            used++;
        }

        faces[nface][0] = (u32)px;
        faces[nface][1] = (u32)(int)((asc - desc) * px_scale + 0.5);
        faces[nface][2] = (u32)(int)(asc * px_scale + 0.5);
        faces[nface][3] = (u32)used;
        faces[nface][4] = blob_off;                         /* коды    */
        faces[nface][5] = blob_off + (u32)used * 4;         /* глифы   */
        faces[nface][6] = faces[nface][5] + (u32)used * GLYPH_REC;  /* пиксели */
        faces[nface][7] = total;

        for (int i = 0; i < used; i++) {
            u32 c = gl[i].code;

            fwrite(&c, 4, 1, out);
        }
        for (int i = 0; i < used; i++) {
            /*
             * Двенадцать байт на знак, а не восемь.
             *
             * Восьми хватало, пока начертания были мелкими: ширина,
             * высота, сдвиг и подъём над базовой линией укладывались в
             * байт каждый. На кегле 190 подъём цифры равен -137, а в
             * знаковый байт помещается только -128 — и он молча
             * превращался в +119. Цифры от этого уезжали на две с
             * лишним сотни точек вниз, и выглядело это так, будто
             * съехало двоеточие, хотя оно как раз стояло верно.
             *
             * Поэтому все метрики теперь шестнадцатибитные. Подпись
             * файла сменена на STF2: старый шрифт с новым ядром не
             * должен молча прочитаться как попало.
             */
            u8 rec[GLYPH_REC];
            int lf = gl[i].left, tp = gl[i].top;

            rec[0] = (u8)(gl[i].off & 0xFF);
            rec[1] = (u8)((gl[i].off >> 8) & 0xFF);
            rec[2] = (u8)((gl[i].off >> 16) & 0xFF);
            rec[3] = (u8)((gl[i].off >> 24) & 0xFF);
            rec[4] = (u8)(gl[i].w & 0xFF);
            rec[5] = (u8)((gl[i].w >> 8) & 0xFF);
            rec[6] = (u8)(gl[i].h & 0xFF);
            rec[7] = (u8)((gl[i].h >> 8) & 0xFF);
            rec[8] = (u8)(gl[i].adv & 0xFF);
            rec[9] = (u8)((gl[i].adv >> 8) & 0xFF);
            rec[10] = (u8)(lf & 0xFF);
            rec[11] = (u8)((lf >> 8) & 0xFF);
            rec[12] = (u8)(tp & 0xFF);
            rec[13] = (u8)((tp >> 8) & 0xFF);
            rec[14] = 0;
            rec[15] = 0;
            fwrite(rec, 1, GLYPH_REC, out);
        }
        for (int i = 0; i < used; i++) {
            if (gl[i].bits) {
                fwrite(gl[i].bits, 1, (size_t)gl[i].w * gl[i].h, out);
                free(gl[i].bits);
                gl[i].bits = NULL;
            }
        }

        /*
         * Дотягиваем до четырёх байт. Ядро читает коды знаков как
         * четырёхбайтовые числа, а на этой архитектуре чтение по
         * невыровненному адресу — не медленное, а запрещённое.
         */
        {
            u32 pad = (4 - (total & 3)) & 3;
            u32 zero = 0;

            if (pad)
                fwrite(&zero, 1, pad, out);
            blob_off = faces[nface][6] + total + pad;
        }
        printf("        %2d px: знаков %d, пикселей %u\n", px, used, total);
        nface++;
    }

    fseek(out, 4, SEEK_SET);
    {
        u32 n = (u32)nface;

        fwrite(&n, 4, 1, out);
        for (int i = 0; i < nface; i++)
            fwrite(faces[i], 4, 8, out);
    }

    fseek(out, 0, SEEK_END);
    printf("fontgen: %s -> %s (%ld байт)\n", argv[1], argv[3], ftell(out));
    fclose(out);
    return 0;
}
