/*
 * Фреймбуфер и текстовая консоль на нём.
 *
 * Главная идея для телефона: LK уже поднял MIPI DSI панель и показывает
 * логотип, то есть контроллер дисплея настроен и сканирует буфер в DRAM.
 * Нам не нужен драйвер DSI — достаточно узнать адрес этого буфера и
 * писать туда пиксели.
 *
 * Адрес берём прямо из регистра оверлея контроллера дисплея (DISP_OVL0):
 * там лежит физический адрес слоя, который прямо сейчас выводится
 * на экран. Это надёжнее, чем угадывать «где-то в верхней памяти».
 */
#include "fb.h"
#include "io.h"
#include "fdt.h"

extern const u8 font8x8[64][8];
extern const u8 font_cyr[33][8];

/*
 * Масштаб глифа подбираем под ширину экрана, а не задаём числом.
 *
 * Смысл в том, чтобы в строку влезало примерно одинаковое число символов
 * на любом экране: на 1080 пикселях телефона это множитель 3, на 720
 * в эмуляторе — 2, и там, и там выходит около 45 знаков. Иначе отладка
 * шла бы на одной плотности текста, а телефон показывал бы другую,
 * и все переносы строк пришлось бы проверять заново.
 */
#define GLYPH_W       (8 * fb.scale)
#define GLYPH_H       (8 * fb.scale)
#define MARGIN        8

static struct {
    volatile u32 *base;
    u32 width, height;
    u32 stride_px;                      /* пикселей в строке (может быть > width) */
    u32 fg, bg;
    u32 cur_x, cur_y;                   /* курсор консоли в пикселях */
    u32 scale;                          /* во сколько раз растянут глиф 8x8 */
    int ready;
} fb;

#if defined(BOARD_MERLIN)
#include "soc/mt6768.h"

/*
 * Адреса DISP_OVL0 и смещения регистров живут в soc/mt6768.h.
 * Базовый адрес 0x1400B000 снят с живого устройства (disp_ovl0@1400b000).
 *
 * Есть и второй, ещё более надёжный источник адреса фреймбуфера:
 * LK кладёт его в передаваемый нам DTB как chosen/atag,videolfb-fb_base_h
 * и ...-fb_base_l. Перейдём на него, когда напишем разбор DTB (этап 5);
 * пока читаем регистр оверлея — он не требует парсера.
 */

/* Чем именно нашли буфер — видно снаружи через диагностику:
 * 0 не нашли, 1 из device tree, 2 из регистра оверлея. */
int fb_probe_source;
u32 fb_lcm_inited;                      /* включил ли LK саму панель */

/*
 * Адрес из device tree — источник, задуманный в проекте с самого начала.
 *
 * LK кладёт в /chosen физический адрес буфера, который сам же и показывает,
 * двумя половинами: atag,videolfb-fb_base_h и -fb_base_l. Это надёжнее
 * чтения регистра оверлея: регистр отражает лишь текущее состояние
 * контроллера, а к моменту передачи управления оно может быть уже сброшено.
 * Из-под Android эти свойства не прочитать, SELinux не пускает shell,
 * но наше ядро работает в EL1 и берёт их свободно.
 */
static int fb_probe_dtb(u64 *addr_out)
{
    u64 dtb = fdt_root();
    u32 hi = 0, lo = 0;

    if (!dtb)
        return -1;
    if (fdt_node_prop_u32(dtb, "chosen", "atag,videolfb-fb_base_h", &hi) != 0)
        return -1;
    if (fdt_node_prop_u32(dtb, "chosen", "atag,videolfb-fb_base_l", &lo) != 0)
        return -1;

    /* Заодно запоминаем, поднял ли загрузчик панель: если нет, писать
     * в память бесполезно, экран останется тёмным при любом адресе. */
    fdt_node_prop_u32(dtb, "chosen", "atag,videolfb-islcm_inited", &fb_lcm_inited);

    *addr_out = ((u64)hi << 32) | lo;
    return 0;
}

/* Отдельно: включил ли загрузчик панель. Если нет, писать в буфер
 * бесполезно при любом адресе. */
static int fb_probe_dtb_lcm(void)
{
    u64 dtb = fdt_root();

    if (!dtb)
        return -1;
    return fdt_node_prop_u32(dtb, "chosen", "atag,videolfb-islcm_inited",
                             &fb_lcm_inited);
}

static int fb_probe(void)
{
    u32 size  = mmio_read32(MT_DISP_OVL0_BASE + OVL_L0_SRC_SIZE);
    u32 pitch = mmio_read32(MT_DISP_OVL0_BASE + OVL_L0_PITCH) & 0xFFFF;
    u32 w = size & 0xFFFF;
    u32 h = (size >> 16) & 0xFFFF;
    u64 addr = 0;

    fb_probe_source = 0;

    /*
     * Сперва регистр оверлея, дерево — запасной вариант.
     *
     * Порядок именно такой по итогам опыта на живом устройстве: с адресом
     * из регистра наша заливка доходила до экрана (логотип загрузчика
     * закрашивался), а с адресом из /chosen логотип оставался нетронутым.
     * То есть в дереве LK оставляет адрес какого-то другого буфера, а
     * показывает он тот, что прописан в оверлее.
     */
    addr = mmio_read32(MT_DISP_OVL0_BASE + OVL_L0_ADDR);
    if (addr >= 0x40000000 && addr <= 0xC0000000) {
        fb_probe_source = 2;
    } else if (fb_probe_dtb(&addr) == 0 &&
               addr >= 0x40000000 && addr < 0x100000000UL) {
        fb_probe_source = 1;
    } else {
        return -1;
    }

    /* Панель нас интересует независимо от источника адреса */
    (void)fb_probe_dtb_lcm();

    /* Размеры: регистру верим только если он выдал правдоподобное.
     * Иначе берём паспортные — панель merlinnfc известна и не меняется. */
    if (w == 0 || h == 0 || w > 4096 || h > 4096) {
        w = MERLIN_FB_WIDTH;
        h = MERLIN_FB_HEIGHT;
    }
    if (pitch < w * 4)
        pitch = w * 4;

    fb.base      = (volatile u32 *)(uintptr_t)addr;
    fb.width     = w;
    fb.height    = h;
    fb.stride_px = pitch / 4;
    return 0;
}

#else
#include "ramfb.h"

/*
 * QEMU -M virt своего экрана не имеет, но умеет ramfb: берёт буфер
 * из нашей же памяти и показывает его как дисплей. Для нас это ровно
 * та же схема, что на телефоне — линейный массив пикселей в DRAM, —
 * поэтому весь код ниже (шрифт, консоль, прокрутка) одинаков для обоих.
 *
 * Размер взят портретный, как у merlin: пусть окно эмулятора выглядит
 * телефоном, а расчёты строк и переносов проверяются на тех же пропорциях.
 *
 * Буфер лежит в .bss, а не выделяется через pmm, по простой причине:
 * экран поднимается ДО аллокатора — иначе первые же сообщения о памяти
 * было бы некуда выводить. Заодно он автоматически попадает в область
 * образа, которую pmm и так помечает занятой.
 */
#define QEMU_FB_W   720
#define QEMU_FB_H   1280

static u32 qemu_fb[QEMU_FB_W * QEMU_FB_H] __attribute__((aligned(4096)));

static int fb_probe(void)
{
    if (ramfb_setup((u64)(uintptr_t)qemu_fb, QEMU_FB_W, QEMU_FB_H,
                    QEMU_FB_W * 4) != 0)
        return -1;                  /* запущено без -device ramfb */

    fb.base      = qemu_fb;
    fb.width     = QEMU_FB_W;
    fb.height    = QEMU_FB_H;
    fb.stride_px = QEMU_FB_W;
    return 0;
}
#endif

int fb_init(void)
{
    fb.ready = 0;
    fb.fg = COLOR_WHITE;
    fb.bg = COLOR_BLACK;
    fb.cur_x = MARGIN;
    fb.cur_y = MARGIN;

    if (fb_probe() != 0)
        return -1;

    /* Около 45 знаков в строке при любой ширине */
    fb.scale = fb.width / (45 * 8);
    if (fb.scale < 1)
        fb.scale = 1;

    fb.ready = 1;
    return 0;
}

int fb_available(void) { return fb.ready; }

void fb_info(u64 *base, u32 *w, u32 *h, u32 *stride)
{
    if (base)   *base   = (u64)(uintptr_t)fb.base;
    if (w)      *w      = fb.width;
    if (h)      *h      = fb.height;
    if (stride) *stride = fb.stride_px;
}

void fb_set_colors(u32 fg, u32 bg) { fb.fg = fg; fb.bg = bg; }

void fb_clear(u32 color)
{
    if (!fb.ready)
        return;
    for (u32 y = 0; y < fb.height; y++) {
        volatile u32 *row = fb.base + (u64)y * fb.stride_px;
        for (u32 x = 0; x < fb.width; x++)
            row[x] = color;
    }
    dsb();
    fb.cur_x = MARGIN;
    fb.cur_y = MARGIN;
}

void fb_fill_rect(u32 x0, u32 y0, u32 w, u32 h, u32 color)
{
    if (!fb.ready)
        return;
    if (x0 >= fb.width || y0 >= fb.height)
        return;
    if (x0 + w > fb.width)  w = fb.width  - x0;
    if (y0 + h > fb.height) h = fb.height - y0;

    for (u32 y = 0; y < h; y++) {
        volatile u32 *row = fb.base + (u64)(y0 + y) * fb.stride_px + x0;
        for (u32 x = 0; x < w; x++)
            row[x] = color;
    }
    dsb();
}

/*
 * Найти глиф по кодовой точке.
 *
 * Латиница лежит подряд с 0x20, кириллица — отдельной таблицей: в юникоде
 * А..Я идут с 0x410, а Ё выбивается из алфавитного порядка и стоит на 0x401,
 * поэтому у неё отдельная запись в конце таблицы.
 *
 * Строчные приводим к прописным: рисовать два начертания в шрифте 8x8
 * негде, а читаемость от этого не страдает.
 */
static const u8 *glyph_for(u32 cp)
{
    if (cp >= 'a' && cp <= 'z')
        cp -= 32;
    if (cp >= 0x20 && cp <= 0x5F)
        return font8x8[cp - 0x20];

    if (cp >= 0x430 && cp <= 0x44F)     /* а..я -> А..Я */
        cp -= 0x20;
    if (cp >= 0x410 && cp <= 0x42F)
        return font_cyr[cp - 0x410];

    if (cp == 0x401 || cp == 0x451)     /* Ё и ё */
        return font_cyr[32];

    return font8x8['?' - 0x20];
}

static void draw_glyph(u32 cp, u32 px, u32 py)
{
    const u8 *glyph = glyph_for(cp);

    for (u32 row = 0; row < 8; row++) {
        u8 bits = glyph[row];
        for (u32 col = 0; col < 8; col++) {
            u32 color = (bits & (0x80 >> col)) ? fb.fg : fb.bg;
            /* пиксель шрифта растягиваем в квадрат scale x scale */
            for (u32 sy = 0; sy < fb.scale; sy++) {
                u32 y = py + row * fb.scale + sy;
                if (y >= fb.height)
                    return;
                volatile u32 *p = fb.base + (u64)y * fb.stride_px
                                + px + col * fb.scale;
                for (u32 sx = 0; sx < fb.scale; sx++)
                    p[sx] = color;
            }
        }
    }
}

/*
 * Вывод символа.
 *
 * На вход приходят БАЙТЫ, а текст ядра в UTF-8, где кириллица занимает два
 * байта. Поэтому здесь маленький автомат: увидев ведущий байт, запоминаем
 * его и ждём второй, и только собрав кодовую точку целиком, рисуем глиф
 * и двигаем курсор. Иначе каждая русская буква съедала бы две позиции
 * и печаталась как два мусорных знака.
 *
 * Поддержаны двухбайтовые последовательности (0xC0..0xDF) — этого хватает
 * на всю кириллицу; более длинные пропускаем, чтобы автомат не залипал.
 */
static void fb_putcp(u32 cp);

void fb_putc(char c)
{
    static u8 lead;
    u8 b = (u8)c;

    if (!fb.ready)
        return;

    if (lead) {
        u8 saved = lead;

        lead = 0;
        if ((b & 0xC0) == 0x80) {
            fb_putcp(((u32)(saved & 0x1F) << 6) | (b & 0x3F));
            return;
        }
        /* Оборванная последовательность: продолжаем разбирать байт как есть */
    }

    if (b >= 0xC0 && b <= 0xDF) {
        lead = b;
        return;
    }
    if (b >= 0xE0) {
        /* Трёх- и четырёхбайтовые нам взять неоткуда — их в тексте ядра нет */
        return;
    }

    fb_putcp(b);
}

static void fb_putcp(u32 cp)
{
    /* Работаем именно с кодовой точкой, а не с char: кириллица начинается
     * с 0x410, и сужение до восьми бит превращало Я (0x42F) в '/' (0x2F). */
    if (cp == '\r') {
        fb.cur_x = MARGIN;
        return;
    }
    if (cp == '\n') {
        fb.cur_x = MARGIN;
        fb.cur_y += GLYPH_H;
        goto wrap;
    }

    if (fb.cur_x + GLYPH_W > fb.width - MARGIN) {
        fb.cur_x = MARGIN;
        fb.cur_y += GLYPH_H;
    }

wrap:
    /* Прокрутка 10-мегабайтного буфера слишком дорога без DMA,
     * поэтому при заполнении экрана просто начинаем сверху заново. */
    if (fb.cur_y + GLYPH_H > fb.height - MARGIN) {
        u32 saved_fg = fb.fg;
        fb_clear(fb.bg);
        fb.fg = saved_fg;
    }
    if (cp == '\n')
        return;

    draw_glyph(cp, fb.cur_x, fb.cur_y);
    fb.cur_x += GLYPH_W;
    dsb();
}
