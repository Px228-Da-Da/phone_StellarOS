/*
 * Слои контроллера дисплея.
 *
 * Устройство слоя: четыре регистра описывают, что показывать (адрес,
 * размер, длина строки), два — где и как (положение, настройки), и ещё
 * два разрешения: бит в общем регистре включения и своё разрешение
 * читать память. Пропустить любое из двух разрешений — слой не появится,
 * причём молча.
 *
 * Все смещения идут с шагом 0x20 от слоя к слою, включая адрес буфера,
 * который лежит далеко от остальных — на 0xF40.
 */
#include "ovl.h"
#include "fb.h"
#include "io.h"
#include "print.h"

#if defined(BOARD_MERLIN)
#include "soc/mt6768.h"

#define OVL_LAYERS  4

static u64 lreg(u32 layer, u32 off)
{
    return MT_DISP_OVL0_BASE + off + (u64)layer * OVL_LAYER_STEP;
}

/*
 * Формат пикселя берём с живого слоя 0.
 *
 * Загрузчик уже настроил его под тот кадр, который панель показывает
 * прямо сейчас, — значит там заведомо верные код формата и порядок байт.
 * Своя таблица форматов тут была бы лишним поводом ошибиться.
 */
static u32 ovl_pixel_format(void)
{
    u32 con = mmio_read32(lreg(0, OVL_L0_CON));

    return con & (OVL_CON_CFMT_MASK | OVL_CON_BTSW);
}

void ovl_dump(void)
{
    u32 src_con = mmio_read32(MT_DISP_OVL0_BASE + OVL_SRC_CON);

    kprintf("OVL: ВКЛЮЧЕНЫ %08x, ОБЛАСТЬ %08x, ФОН %08x\n",
            src_con,
            mmio_read32(MT_DISP_OVL0_BASE + OVL_ROI_SIZE),
            mmio_read32(MT_DISP_OVL0_BASE + OVL_ROI_BGCLR));

    for (u32 i = 0; i < OVL_LAYERS; i++) {
        u32 con = mmio_read32(lreg(i, OVL_L0_CON));

        kprintf("OVL СЛОЙ %u %s: НАСТР %08x ФОРМАТ %u АЛЬФА %u\n",
                i, (src_con & (1U << i)) ? "ВКЛ " : "ВЫКЛ",
                con, (con & OVL_CON_CFMT_MASK) >> OVL_CON_CFMT_SHIFT,
                con & OVL_CON_ALPHA_MASK);
        kprintf("         РАЗМЕР %08x МЕСТО %08x ШАГ %08x АДРЕС %08x ЧТЕНИЕ %u\n",
                mmio_read32(lreg(i, OVL_L0_SRC_SIZE)),
                mmio_read32(lreg(i, OVL_L0_OFFSET)),
                mmio_read32(lreg(i, OVL_L0_PITCH)),
                mmio_read32(lreg(i, OVL_L0_ADDR)),
                mmio_read32(lreg(i, OVL_RDMA0_CTRL)) & 1);
    }
}

/* Включить или выключить слой в общем регистре.
 * Читаем-меняем-пишем только свой бит: соседние слои трогать нельзя. */
static void ovl_enable(u32 layer, int on)
{
    u32 v = mmio_read32(MT_DISP_OVL0_BASE + OVL_SRC_CON);

    if (on)
        v |= (1U << layer);
    else
        v &= ~(1U << layer);
    mmio_write32(MT_DISP_OVL0_BASE + OVL_SRC_CON, v);
    mmio_write32(lreg(layer, OVL_RDMA0_CTRL), on ? 1 : 0);
    dsb();
}

int ovl_layer_set(u32 layer, const volatile void *buf,
                  u32 x, u32 y, u32 w, u32 h, u32 pitch, u32 alpha)
{
    u32 con;

    if (layer == 0 || layer >= OVL_LAYERS || !buf || !w || !h)
        return -1;
    if (alpha > 255)
        alpha = 255;

    /* Настройки меняем в окне между кадрами: попадём в середину вывода —
     * увидим шов, ровно как это было с подменой адреса. */
    fb_wait_frame_gap();

    con = ovl_pixel_format()
        | OVL_CON_AEN                       /* прозрачность учитывается */
        | (alpha & OVL_CON_ALPHA_MASK)
        | (0U << OVL_CON_LSRC_SHIFT);       /* источник — память        */

    mmio_write32(lreg(layer, OVL_L0_CON), con);
    mmio_write32(lreg(layer, OVL_L0_SRC_SIZE), (h << 16) | w);
    mmio_write32(lreg(layer, OVL_L0_OFFSET), (y << 16) | x);
    mmio_write32(lreg(layer, OVL_L0_PITCH), pitch & 0xFFFF);
    mmio_write32(lreg(layer, OVL_L0_CLIP), 0);
    mmio_write32(lreg(layer, OVL_L0_SRCKEY), 0);
    mmio_write32(lreg(layer, OVL_L0_ADDR), (u32)(uintptr_t)buf);
    dsb();

    ovl_enable(layer, 1);
    return 0;
}

/*
 * Слой сплошного цвета.
 *
 * Буфер не нужен вовсе: цвет лежит в отдельном регистре, а источником
 * слоя выбирается не память. Читать нечего, значит и полосы пропускной
 * способности такой слой не занимает — затемнить им фон под диалогом
 * стоит ровно ноль.
 */
int ovl_layer_color(u32 layer, u32 argb, u32 x, u32 y, u32 w, u32 h)
{
    u32 con;

    if (layer == 0 || layer >= OVL_LAYERS || !w || !h)
        return -1;

    fb_wait_frame_gap();

    /* Прозрачность слоя берём из старшего байта цвета: так вызывающему
     * не нужно помнить, что она задаётся отдельно от него. */
    con = ovl_pixel_format()
        | OVL_CON_AEN
        | ((argb >> 24) & OVL_CON_ALPHA_MASK)
        | (1U << OVL_CON_LSRC_SHIFT);       /* источник — сплошной цвет */

    mmio_write32(MT_DISP_OVL0_BASE + OVL_L0_CLR + layer * 4, argb);
    mmio_write32(lreg(layer, OVL_L0_CON), con);
    mmio_write32(lreg(layer, OVL_L0_SRC_SIZE), (h << 16) | w);
    mmio_write32(lreg(layer, OVL_L0_OFFSET), (y << 16) | x);
    mmio_write32(lreg(layer, OVL_L0_CLIP), 0);
    dsb();

    ovl_enable(layer, 1);
    return 0;
}

void ovl_layer_move(u32 layer, u32 x, u32 y)
{
    if (layer == 0 || layer >= OVL_LAYERS)
        return;
    mmio_write32(lreg(layer, OVL_L0_OFFSET), (y << 16) | x);
    dsb();
}

void ovl_layer_alpha(u32 layer, u32 alpha)
{
    u32 con;

    if (layer == 0 || layer >= OVL_LAYERS)
        return;
    con = mmio_read32(lreg(layer, OVL_L0_CON));
    con = (con & ~OVL_CON_ALPHA_MASK) | (alpha & OVL_CON_ALPHA_MASK);
    mmio_write32(lreg(layer, OVL_L0_CON), con);
    dsb();
}

void ovl_layer_off(u32 layer)
{
    if (layer == 0 || layer >= OVL_LAYERS)
        return;
    fb_wait_frame_gap();
    ovl_enable(layer, 0);
}

#else   /* в эмуляторе слоёв нет: там кадр один и складывать нечего */

void ovl_dump(void) { }
int  ovl_layer_set(u32 l, const volatile void *b, u32 x, u32 y,
                   u32 w, u32 h, u32 p, u32 a)
{
    (void)l; (void)b; (void)x; (void)y; (void)w; (void)h; (void)p; (void)a;
    return -1;
}
int  ovl_layer_color(u32 l, u32 c, u32 x, u32 y, u32 w, u32 h)
{
    (void)l; (void)c; (void)x; (void)y; (void)w; (void)h;
    return -1;
}
void ovl_layer_move(u32 l, u32 x, u32 y) { (void)l; (void)x; (void)y; }
void ovl_layer_alpha(u32 l, u32 a) { (void)l; (void)a; }
void ovl_layer_off(u32 l) { (void)l; }

#endif
