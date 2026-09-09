/*
 * Шрифт: разбор того, что приготовил tools/fontgen.
 *
 * Работы здесь ровно на один проход по заголовку — и это главное свойство
 * решения. Всё дорогое (чтение TrueType, кривые Безье, сглаживание)
 * сделано на компьютере при сборке; ядру достались пиксели и таблица,
 * где какой знак лежит.
 *
 * Заодно это проверка: блоб приходит из образа, но всякий разбор данных
 * должен доверять только тому, что сам сверил. Смещения проверяются на
 * попадание в блоб, иначе испорченный шрифт увёл бы вывод текста в чужую
 * память — а текст ядро рисует и в панике, когда ошибаться уже некуда.
 */
#include "font.h"
#include "print.h"

#define FACE_MAX 10

static struct font_face faces[FACE_MAX];
static u32 nfaces;

/* Блоб little-endian, как и всё у нас; выравнивание обеспечил fontgen */
static u32 rd32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

int font_init(const u8 *blob, u32 len)
{
    u32 n, i;

    nfaces = 0;

    if (!blob || len < 8 + FACE_MAX * 32)
        return -1;
    /* Подпись STF2: у первого формата метрики были однобайтовыми и
     * переполнялись на крупных начертаниях. Прочитать старый файл новым
     * разборщиком нельзя, и молчать об этом нельзя тем более. */
    if (blob[0] != 'S' || blob[1] != 'T' || blob[2] != 'F' || blob[3] != '2') {
        kprintf("ШРИФТ    : ЭТО НЕ ШРИФТ StellarOS\n");
        return -1;
    }

    n = rd32(blob + 4);
    if (n > FACE_MAX)
        return -1;

    for (i = 0; i < n; i++) {
        const u8 *f = blob + 8 + i * 32;
        u32 px = rd32(f), line = rd32(f + 4), base = rd32(f + 8);
        u32 count = rd32(f + 12);
        u32 codes = rd32(f + 16), glyphs = rd32(f + 20);
        u32 bits = rd32(f + 24), bits_len = rd32(f + 28);

        /* Всё, что обещает заголовок, обязано лежать внутри блоба */
        if (!count || count > 4096 ||
            codes + count * 4 > len ||
            glyphs + count * 16 > len ||
            bits + bits_len > len) {
            kprintf("ШРИФТ    : НАЧЕРТАНИЕ %u ВЫХОДИТ ЗА ГРАНИЦЫ ФАЙЛА\n", px);
            return -1;
        }

        faces[nfaces].px = px;
        faces[nfaces].line = line;
        faces[nfaces].base = base;
        faces[nfaces].count = count;
        faces[nfaces].codes = (const u32 *)(const void *)(blob + codes);
        faces[nfaces].glyphs = blob + glyphs;
        faces[nfaces].bits = blob + bits;
        nfaces++;
    }

    kprintf("ШРИФТ    : MANROPE, НАЧЕРТАНИЙ %u, ЗНАКОВ %u, %u КБ\n",
            nfaces, nfaces ? faces[0].count : 0, len / 1024);
    return 0;
}

const struct font_face *font_pick(u32 px)
{
    const struct font_face *best = 0;
    u32 bestd = 0;

    for (u32 i = 0; i < nfaces; i++) {
        u32 d = faces[i].px > px ? faces[i].px - px : px - faces[i].px;

        if (!best || d < bestd) {
            best = &faces[i];
            bestd = d;
        }
    }

    return best;
}

/*
 * Знак ищем двоичным поиском: коды лежат по возрастанию, знаков под две
 * сотни, а рисование текста — самый частый путь вывода в системе.
 */
int font_glyph(const struct font_face *f, u32 cp, struct font_glyph *out)
{
    u32 lo = 0, hi;

    if (!f || !out || !f->count)
        return -1;

    hi = f->count - 1;
    while (lo <= hi) {
        u32 mid = (lo + hi) / 2;
        u32 c = f->codes[mid];

        if (c == cp) {
            /*
             * Шестнадцать байт на знак, и все метрики шестнадцатибитные.
             *
             * Было восемь, по байту на поле, и на мелких начертаниях это
             * работало. На кегле 190 подъём цифры над базовой линией
             * равен -137, а в знаковый байт помещается только -128 — и
             * он молча становился +119. Цифры уезжали на две с лишним
             * сотни точек вниз, и снаружи это выглядело так, будто
             * съехало двоеточие, хотя оно как раз стояло верно. Ошибка,
             * которую невозможно увидеть по коду: обе стороны были
             * согласованы между собой и обе ошибались одинаково.
             */
            const u8 *g = f->glyphs + mid * 16;
            u32 off = (u32)g[0] | ((u32)g[1] << 8) |
                      ((u32)g[2] << 16) | ((u32)g[3] << 24);

            out->w = (u32)g[4] | ((u32)g[5] << 8);
            out->h = (u32)g[6] | ((u32)g[7] << 8);
            out->adv = (u32)g[8] | ((u32)g[9] << 8);
            out->left = (short)((u16)g[10] | ((u16)g[11] << 8));
            out->top = (short)((u16)g[12] | ((u16)g[13] << 8));
            out->bits = (out->w && out->h) ? f->bits + off : 0;
            return 0;
        }
        if (c < cp)
            lo = mid + 1;
        else if (!mid)
            break;
        else
            hi = mid - 1;
    }

    return -1;
}
