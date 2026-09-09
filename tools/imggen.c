/*
 * imggen — картинка в тот вид, который система умеет показать.
 *
 * Работает на компьютере, при сборке. В телефон едет уже разобранная
 * картинка, а не PNG.
 *
 * Почему так, а не разбор на самом телефоне. Разбор PNG — это inflate, то
 * есть таблицы Хаффмана и скользящее окно; разбор JPEG — ещё и обратное
 * косинусное преобразование. Вместе это пара тысяч строк в системе,
 * которая рисует прямоугольниками. И главное: сейчас телефону НЕОТКУДА
 * взять картинку, которой не было при сборке — файловой системы нет,
 * провод не работает, приложения вшиты в образ. Разбор на телефоне
 * распаковывал бы ровно те файлы, что у нас и так есть, только дороже.
 *
 * Когда появится способ доставлять файлы, вернуться к этому будет
 * осмысленно — рисование к тому времени уже будет готово.
 *
 * PNG разбираем сами, зная, что zlib на сборочной машине есть. Форматы
 * помимо PNG приводит к PNG ffmpeg — это делает Makefile, сюда всё
 * приходит уже одним форматом.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

typedef unsigned char  u8;
typedef unsigned int   u32;

static u32 be32(const u8 *p)
{
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

static void die(const char *why)
{
    fprintf(stderr, "imggen: %s\n", why);
    exit(1);
}

/* --- Разбор PNG ------------------------------------------------------ */

static u32 img_w, img_h;
static u32 *pix;                    /* 0xAARRGGBB, как на экране */

/*
 * Восстановление строки из фильтрованной.
 *
 * PNG хранит не сами байты, а разность с соседями — так они лучше
 * сжимаются. Пять способов, и в каждой строке свой, поэтому байт фильтра
 * идёт перед строкой.
 */
static void unfilter(u8 *cur, const u8 *prev, u32 len, u32 bpp, u8 type)
{
    switch (type) {
    case 0:
        break;
    case 1:                         /* слева */
        for (u32 i = bpp; i < len; i++)
            cur[i] = (u8)(cur[i] + cur[i - bpp]);
        break;
    case 2:                         /* сверху */
        for (u32 i = 0; i < len; i++)
            cur[i] = (u8)(cur[i] + prev[i]);
        break;
    case 3:                         /* среднее слева и сверху */
        for (u32 i = 0; i < len; i++) {
            u32 a = i >= bpp ? cur[i - bpp] : 0;

            cur[i] = (u8)(cur[i] + (a + prev[i]) / 2);
        }
        break;
    case 4: {                       /* предсказатель Пита */
        for (u32 i = 0; i < len; i++) {
            int a = i >= bpp ? cur[i - bpp] : 0;
            int b = prev[i];
            int c = i >= bpp ? prev[i - bpp] : 0;
            int p = a + b - c;
            int pa = p > a ? p - a : a - p;
            int pb = p > b ? p - b : b - p;
            int pc = p > c ? p - c : c - p;
            int pr = (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);

            cur[i] = (u8)(cur[i] + pr);
        }
        break;
    }
    default:
        die("незнакомый фильтр строки");
    }
}

static void png_load(const char *path)
{
    static const u8 sig[8] = { 137, 'P', 'N', 'G', 13, 10, 26, 10 };
    FILE *f = fopen(path, "rb");
    u8 *file, *idat = NULL, *raw, *pal = NULL, *trns = NULL;
    long flen;
    u32 idat_len = 0, pal_len = 0, trns_len = 0;
    u32 depth = 0, ctype = 99, chan, bpp, stride;
    unsigned long raw_len;
    u32 pos;

    if (!f)
        die("не открыть картинку");
    fseek(f, 0, SEEK_END);
    flen = ftell(f);
    fseek(f, 0, SEEK_SET);
    file = malloc((size_t)flen);
    if (fread(file, 1, (size_t)flen, f) != (size_t)flen)
        die("файл читается не целиком");
    fclose(f);

    if (flen < 8 || memcmp(file, sig, 8) != 0)
        die("это не PNG");

    /* Проходим по кускам: заголовок, палитра, данные */
    pos = 8;
    while (pos + 8 <= (u32)flen) {
        u32 len = be32(file + pos);
        const char *type = (const char *)file + pos + 4;
        u8 *data = file + pos + 8;

        if (!memcmp(type, "IHDR", 4)) {
            img_w = be32(data);
            img_h = be32(data + 4);
            depth = data[8];
            ctype = data[9];
            if (data[12] != 0)
                die("чересстрочный PNG не поддержан");
            if (depth != 8)
                die("поддержана только глубина 8 бит на канал");
        } else if (!memcmp(type, "PLTE", 4)) {
            pal = data;
            pal_len = len;
        } else if (!memcmp(type, "tRNS", 4)) {
            trns = data;
            trns_len = len;
        } else if (!memcmp(type, "IDAT", 4)) {
            idat = realloc(idat, idat_len + len);
            memcpy(idat + idat_len, data, len);
            idat_len += len;
        } else if (!memcmp(type, "IEND", 4)) {
            break;
        }
        pos += 12 + len;
    }

    if (!idat || !img_w || !img_h || ctype == 99)
        die("в файле нет картинки");
    (void)depth;

    switch (ctype) {
    case 0: chan = 1; break;        /* серый            */
    case 2: chan = 3; break;        /* RGB              */
    case 3: chan = 1; break;        /* по палитре       */
    case 4: chan = 2; break;        /* серый с прозрачностью */
    case 6: chan = 4; break;        /* RGBA             */
    default: die("незнакомый вид цвета"); return;
    }
    if (ctype == 3 && !pal)
        die("по палитре, а палитры нет");

    bpp = chan;
    stride = img_w * chan;
    raw_len = (unsigned long)img_h * (stride + 1);
    raw = malloc(raw_len);
    if (uncompress(raw, &raw_len, idat, idat_len) != Z_OK)
        die("данные не распаковались");

    pix = malloc((size_t)img_w * img_h * 4);

    {
        u8 *prev = calloc(stride, 1);

        for (u32 y = 0; y < img_h; y++) {
            u8 *line = raw + (unsigned long)y * (stride + 1);
            u8 filt = line[0];
            u8 *cur = line + 1;

            unfilter(cur, prev, stride, bpp, filt);

            for (u32 x = 0; x < img_w; x++) {
                u8 r, g, b, a = 255;
                const u8 *s = cur + (size_t)x * chan;

                switch (ctype) {
                case 0: r = g = b = s[0]; break;
                case 4: r = g = b = s[0]; a = s[1]; break;
                case 2: r = s[0]; g = s[1]; b = s[2]; break;
                case 6: r = s[0]; g = s[1]; b = s[2]; a = s[3]; break;
                default: {          /* по палитре */
                    u32 i = s[0];

                    if (i * 3 + 2 >= pal_len)
                        die("указатель за палитру");
                    r = pal[i * 3];
                    g = pal[i * 3 + 1];
                    b = pal[i * 3 + 2];
                    if (trns && i < trns_len)
                        a = trns[i];
                    break;
                }
                }
                pix[(size_t)y * img_w + x] =
                    ((u32)a << 24) | ((u32)r << 16) | ((u32)g << 8) | b;
            }
            memcpy(prev, cur, stride);
        }
        free(prev);
    }
}

/* --- Запись .sti ----------------------------------------------------- */

/*
 * Формат простой нарочно: показывать его будет система, у которой нет ни
 * распаковщиков, ни лишней памяти.
 *
 *   "STI1"    подпись
 *   u32 w, h
 *   u32 flags   бит 0: строки сжаты повторами
 *   u32 len     сколько байт данных
 *   данные
 *
 * Сжатие — повторами одинаковых точек: байт длины (1..255) и сама точка,
 * пять байт на серию. Для значков, где большие ровные заливки, это
 * выигрыш в десятки раз. Для фотографии — проигрыш, поэтому пишем то из
 * двух, что вышло короче, а какое именно, говорит флаг.
 */
static u32 *rle_pack(u32 *n_out)
{
    u32 total = img_w * img_h;
    u32 *out = malloc((size_t)total * 8 + 16);
    u8 *b = (u8 *)out;
    u32 n = 0, i = 0;

    while (i < total) {
        u32 v = pix[i];
        u32 run = 1;

        while (run < 255 && i + run < total && pix[i + run] == v)
            run++;
        b[n++] = (u8)run;
        memcpy(b + n, &v, 4);
        n += 4;
        i += run;
    }
    *n_out = n;
    return out;
}

/*
 * Уменьшить до ширины tw, усредняя.
 *
 * Усреднение, а не выбрасывание лишних точек: обои это плавные переходы,
 * и выбрасывание оставило бы на них ступеньки. Считаем каждую точку
 * нового изображения как среднее прямоугольника старых, что на неё
 * приходится, — то есть настоящее уменьшение, а не прореживание.
 *
 * Складываем в u32 и делим в конце: суммировать до деления обязательно,
 * иначе на каждой точке терялись бы младшие разряды, и на градиенте это
 * видно полосами.
 */
static void downscale(u32 tw)
{
    u32 th, *dst;

    if (!tw || tw >= img_w)
        return;
    th = (u32)(((unsigned long long)img_h * tw + img_w / 2) / img_w);
    if (!th)
        th = 1;

    dst = malloc((size_t)tw * th * 4);
    if (!dst)
        die("не хватило памяти на уменьшение");

    for (u32 y = 0; y < th; y++) {
        u32 y0 = (u32)((unsigned long long)y * img_h / th);
        u32 y1 = (u32)((unsigned long long)(y + 1) * img_h / th);

        if (y1 <= y0)
            y1 = y0 + 1;
        for (u32 x = 0; x < tw; x++) {
            u32 x0 = (u32)((unsigned long long)x * img_w / tw);
            u32 x1 = (u32)((unsigned long long)(x + 1) * img_w / tw);
            u32 a = 0, r = 0, g = 0, b = 0, n = 0;

            if (x1 <= x0)
                x1 = x0 + 1;
            for (u32 sy = y0; sy < y1; sy++)
                for (u32 sx = x0; sx < x1; sx++) {
                    u32 p = pix[(size_t)sy * img_w + sx];

                    a += p >> 24 & 0xFF;
                    r += p >> 16 & 0xFF;
                    g += p >>  8 & 0xFF;
                    b += p       & 0xFF;
                    n++;
                }
            dst[(size_t)y * tw + x] = ((a / n) << 24) | ((r / n) << 16)
                                    | ((g / n) << 8) | (b / n);
        }
    }

    free(pix);
    pix = dst;
    img_w = tw;
    img_h = th;
}

int main(int argc, char **argv)
{
    FILE *out;
    u32 head[4];
    u32 raw_bytes, rle_bytes;
    u32 *rle;

    if (argc != 3 && argc != 4) {
        fprintf(stderr,
                "как пользоваться: imggen картинка.png картинка.sti [ширина]\n"
                "  ширина — уменьшить до неё, сохранив пропорции\n");
        return 1;
    }

    png_load(argv[1]);
    if (argc == 4)
        downscale((u32)strtoul(argv[3], NULL, 10));
    raw_bytes = img_w * img_h * 4;
    rle = rle_pack(&rle_bytes);

    out = fopen(argv[2], "wb");
    if (!out)
        die("не создать выходной файл");

    head[0] = img_w;
    head[1] = img_h;
    head[2] = rle_bytes < raw_bytes ? 1u : 0u;
    head[3] = rle_bytes < raw_bytes ? rle_bytes : raw_bytes;

    fwrite("STI1", 1, 4, out);
    fwrite(head, 4, 4, out);
    fwrite(head[2] ? (void *)rle : (void *)pix, 1, head[3], out);
    fclose(out);

    printf("imggen: %s -> %s (%ux%u, %s, %u байт вместо %u)\n",
           argv[1], argv[2], img_w, img_h,
           head[2] ? "повторами" : "как есть", head[3], raw_bytes);
    return 0;
}
