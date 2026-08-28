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

extern const u8 font8x8[64][8];

#define GLYPH_SCALE   3                 /* 8x8 -> 24x24, читаемо на 1080p */
#define GLYPH_W       (8 * GLYPH_SCALE)
#define GLYPH_H       (8 * GLYPH_SCALE)
#define MARGIN        8

static struct {
    volatile u32 *base;
    u32 width, height;
    u32 stride_px;                      /* пикселей в строке (может быть > width) */
    u32 fg, bg;
    u32 cur_x, cur_y;                   /* курсор консоли в пикселях */
    int ready;
} fb;

#if defined(BOARD_MERLIN)
#include "soc/mt6769.h"

/* Регистры MediaTek DISP_OVL — смещения одинаковы во всём семействе
 * (см. drivers/gpu/drm/mediatek/mtk_disp_ovl.c) */
#define MT_DISP_OVL0_BASE       0x14008000UL    /* TODO-VERIFY по merlin DTS */
#define OVL_L0_SRC_SIZE         0x0038          /* [31:16]=h, [15:0]=w        */
#define OVL_L0_PITCH            0x0044          /* байт в строке              */
#define OVL_L0_ADDR             0x0F40          /* физический адрес буфера    */

static int fb_probe(void)
{
    u32 size  = mmio_read32(MT_DISP_OVL0_BASE + OVL_L0_SRC_SIZE);
    u32 pitch = mmio_read32(MT_DISP_OVL0_BASE + OVL_L0_PITCH) & 0xFFFF;
    u32 addr  = mmio_read32(MT_DISP_OVL0_BASE + OVL_L0_ADDR);
    u32 w = size & 0xFFFF;
    u32 h = (size >> 16) & 0xFFFF;

    /* Санитарная проверка: не поверим мусору и не запишем куда попало */
    if (addr < 0x40000000 || addr > 0xC0000000)
        return -1;
    if (w == 0 || h == 0 || w > 4096 || h > 4096)
        return -1;
    if (pitch < w * 4)
        pitch = w * 4;

    fb.base      = (volatile u32 *)(uintptr_t)addr;
    fb.width     = w;
    fb.height    = h;
    fb.stride_px = pitch / 4;
    return 0;
}
#else
/* QEMU -M virt экрана не имеет: консоль уходит только в UART.
 * Так и задумано — на этапе отладки логики графика не нужна. */
static int fb_probe(void)
{
    return -1;
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

static void draw_glyph(char c, u32 px, u32 py)
{
    u8 idx;

    if (c >= 'a' && c <= 'z')           /* строчных в шрифте нет */
        c -= 32;
    if (c < 0x20 || c > 0x5F)
        c = '?';
    idx = (u8)c - 0x20;

    for (u32 row = 0; row < 8; row++) {
        u8 bits = font8x8[idx][row];
        for (u32 col = 0; col < 8; col++) {
            u32 color = (bits & (0x80 >> col)) ? fb.fg : fb.bg;
            /* масштабируем пиксель шрифта в квадрат GLYPH_SCALE x GLYPH_SCALE */
            for (u32 sy = 0; sy < GLYPH_SCALE; sy++) {
                u32 y = py + row * GLYPH_SCALE + sy;
                if (y >= fb.height)
                    return;
                volatile u32 *p = fb.base + (u64)y * fb.stride_px
                                + px + col * GLYPH_SCALE;
                for (u32 sx = 0; sx < GLYPH_SCALE; sx++)
                    p[sx] = color;
            }
        }
    }
}

void fb_putc(char c)
{
    if (!fb.ready)
        return;

    if (c == '\r') {
        fb.cur_x = MARGIN;
        return;
    }
    if (c == '\n') {
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
    if (c == '\n')
        return;

    draw_glyph(c, fb.cur_x, fb.cur_y);
    fb.cur_x += GLYPH_W;
    dsb();
}
