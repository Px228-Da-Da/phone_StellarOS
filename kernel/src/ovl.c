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
 *
 * Момент правки регистров эти функции НЕ выбирают: это забота вызывающего.
 * Причина в том, что правок обычно несколько, а окно между кадрами одно и
 * очень короткое; ждать его внутри каждой функции значило бы растянуть
 * пачку правок на несколько кадров, и часть из них всё равно легла бы
 * посреди вывода. Исключение — ovl_take_over и ovl_base_layer: они
 * вызываются поодиночке и вне общего цикла.
 */
#include "ovl.h"
#include "backlight.h"
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
 * Прижать слой к границам области вывода.
 *
 * Слой обязан помещаться в неё целиком. Стоит ему вылезти за край — и
 * оверлей показывает не то и не там: на устройстве это выглядело как
 * обрезанный кусок слоя, появляющийся выше пальца, стоило коснуться
 * правого края экрана.
 *
 * Ограничение железное, поэтому и проверка здесь, в драйвере, а не у
 * каждого, кто двигает слой: иначе о него спотыкались бы по очереди все.
 *
 * Размер области читаем у самого оверлея, а не берём из настроек кадра:
 * это разные вещи, и совпадать они не обязаны.
 */
static void clamp_to_roi(u32 *x, u32 *y, u32 w, u32 h)
{
    u32 roi = mmio_read32(MT_DISP_OVL0_BASE + OVL_ROI_SIZE);
    u32 rw = roi & 0xFFFF;
    u32 rh = (roi >> 16) & 0xFFFF;

    if (!rw || !rh)
        return;
    if (w > rw)
        w = rw;
    if (h > rh)
        h = rh;
    if (*x + w > rw)
        *x = rw - w;
    if (*y + h > rh)
        *y = rh - h;
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
        kprintf("         ОЧЕРЕДЬ %08x ПОРОГИ %08x/%08x\n",
                mmio_read32(lreg(i, OVL_RDMA0_FIFO)),
                mmio_read32(lreg(i, OVL_RDMA0_GMC)),
                mmio_read32(MT_DISP_OVL0_BASE + OVL_RDMA0_GMC_S2 + i * 4));
        kprintf("         РАЗМЕР %08x МЕСТО %08x ШАГ %08x АДРЕС %08x ЧТЕНИЕ %u\n",
                mmio_read32(lreg(i, OVL_L0_SRC_SIZE)),
                mmio_read32(lreg(i, OVL_L0_OFFSET)),
                mmio_read32(lreg(i, OVL_L0_PITCH)),
                mmio_read32(lreg(i, OVL_L0_ADDR)),
                mmio_read32(lreg(i, OVL_RDMA0_CTRL)) & 1);
    }
}

/*
 * Скопировать со слоя 0 настройки доступа к памяти.
 *
 * Включить слой и задать ему адрес недостаточно: у каждого слоя есть свои
 * пороги выборки и свой размер очереди, и загрузчик задаёт их только тем
 * слоям, которыми пользуется сам. У остальных там нули — очередь нулевого
 * размера, читать нечем. Слой при этом выглядит полностью настроенным:
 * адрес на месте, размер верный, чтение разрешено, а на экране ничего.
 *
 * Значения не выдумываем, а берём с рабочего слоя 0 — там они заведомо
 * подходят этой панели и этой полосе пропускания. Тот же приём, что и с
 * форматом пикселя.
 */
static void ovl_copy_fetch_setup(u32 layer)
{
    mmio_write32(lreg(layer, OVL_RDMA0_GMC),
                 mmio_read32(lreg(0, OVL_RDMA0_GMC)));
    mmio_write32(lreg(layer, OVL_RDMA0_SLOW),
                 mmio_read32(lreg(0, OVL_RDMA0_SLOW)));
    mmio_write32(lreg(layer, OVL_RDMA0_FIFO),
                 mmio_read32(lreg(0, OVL_RDMA0_FIFO)));
    /* У этого регистра шаг между слоями четыре байта, а не 0x20 */
    mmio_write32(MT_DISP_OVL0_BASE + OVL_RDMA0_GMC_S2 + layer * 4,
                 mmio_read32(MT_DISP_OVL0_BASE + OVL_RDMA0_GMC_S2));
    dsb();
}

/* Включить или выключить слой в общем регистре.
 * Читаем-меняем-пишем только свой бит: соседние слои трогать нельзя. */
static void ovl_enable(u32 layer, int on)
{
    u32 v = mmio_read32(MT_DISP_OVL0_BASE + OVL_SRC_CON);

    if (on) {
        v |= (1U << layer);
        ovl_copy_fetch_setup(layer);
    }
    else
        v &= ~(1U << layer);
    mmio_write32(MT_DISP_OVL0_BASE + OVL_SRC_CON, v);
    mmio_write32(lreg(layer, OVL_RDMA0_CTRL), on ? 1 : 0);
    dsb();
}

void ovl_take_over(void)
{
    u32 src_con = mmio_read32(MT_DISP_OVL0_BASE + OVL_SRC_CON);
    u32 leftover = src_con & 0xE;       /* всё, кроме нулевого слоя */

    if (!leftover)
        return;

    kprintf("OVL: УБИРАЮ СЛОИ ЗАГРУЗЧИКА, БЫЛО ВКЛЮЧЕНО %08x\n", src_con);
    fb_wait_frame_gap();
    for (u32 i = 1; i < OVL_LAYERS; i++)
        if (leftover & (1U << i))
            ovl_enable(i, 0);
}

void ovl_base_layer(int on)
{
    fb_wait_frame_gap();
    ovl_enable(0, on);
}

int ovl_layer_set(u32 layer, const volatile void *buf,
                  u32 x, u32 y, u32 w, u32 h, u32 pitch, u32 alpha)
{
    u32 con;

    if (layer == 0 || layer >= OVL_LAYERS || !buf || !w || !h)
        return -1;
    if (alpha > 255)
        alpha = 255;
    clamp_to_roi(&x, &y, w, h);

    /*
     * Учёт прозрачности включаем ТОЛЬКО если слой не полностью
     * непрозрачен.
     *
     * Раньше он включался всегда, и это оказалось не безобидно. При
     * включённом смешивании железо берёт прозрачность из самого
     * пикселя, а разложение байтов в буфере и в его понимании может не
     * совпадать: наш байт прозрачности прочитывается как цвет, а цвет —
     * как прозрачность. Наружу это выглядит как окно, сквозь которое
     * просвечивает кадр под ним, и меняет вид при каждом изменении
     * этого кадра.
     *
     * Непрозрачному слою смешивание не нужно по определению, а
     * полупрозрачному оно и задаётся отдельно.
     */
    con = ovl_pixel_format()
        | (alpha < 255 ? OVL_CON_AEN : 0)
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

    clamp_to_roi(&x, &y, w, h);

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

void ovl_layer_addr(u32 layer, const volatile void *buf)
{
    if (layer == 0 || layer >= OVL_LAYERS || !buf)
        return;

    mmio_write32(lreg(layer, OVL_L0_ADDR), (u32)(uintptr_t)buf);
    dsb();
}

void ovl_layer_move(u32 layer, u32 x, u32 y)
{
    u32 size;

    if (layer == 0 || layer >= OVL_LAYERS)
        return;

    /* Размер слоя спрашиваем у него самого: вызывающий его уже сообщал
     * при настройке, и заставлять помнить второй раз незачем. */
    size = mmio_read32(lreg(layer, OVL_L0_SRC_SIZE));
    clamp_to_roi(&x, &y, size & 0xFFFF, (size >> 16) & 0xFFFF);

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

/*
 * Погасить экран, не выключая телефон.
 *
 * Гасим тем, что снимаем со всех слоёв разрешение показывать: оверлей
 * отдаёт панели свой фоновый цвет, то есть чёрный. Кадр при этом никуда
 * не девается — он лежит в памяти, слои настроены, и возврат стоит одной
 * записи в тот же регистр.
 *
 * Одних слоёв мало: без подсветки экран становится чёрным, но светится,
 * и питание берёт прежнее — именно это и было видно. Поэтому гасим ещё
 * и подсветку, см. backlight.c. Порядок там важен и оба раза один и тот
 * же: сначала темнота, потом изображение.
 */
static u32 blank_saved_src;
static int blanked;

void ovl_blank(int on)
{
    u32 src;

    if (!!on == blanked)
        return;

    if (on) {
        /*
         * Подсветку снимаем первой. Слои сходят не мгновенно — им нужен
         * промежуток между кадрами, — и при горящей подсветке этот
         * промежуток был бы виден вспышкой.
         */
        backlight_set(0);

        blank_saved_src = mmio_read32(MT_DISP_OVL0_BASE + OVL_SRC_CON);
        /*
         * Ждём промежутка между кадрами.
         *
         * Снять слои посреди кадра значит показать панели половину
         * старого поверх половины чёрного — рваную полосу. Тот же
         * промежуток, в который мы переключаем половины буфера.
         */
        fb_wait_frame_gap();
        mmio_write32(MT_DISP_OVL0_BASE + OVL_SRC_CON,
                     blank_saved_src & ~0xFu);
        blanked = 1;
        kprintf("ЭКРАН    : ПОГАШЕН (ПОДСВЕТКА %s)\n",
                backlight_known() ? "ВЫКЛЮЧЕНА" : "ГОРИТ, УПРАВЛЕНИЯ НЕТ");
    } else {
        /*
         * Возвращаем в обратном порядке: сперва слои, и только когда на
         * панель уже пошёл кадр — подсветку. Иначе первым, что увидел бы
         * человек, был бы чёрный экран включающейся подсветки.
         */
        src = mmio_read32(MT_DISP_OVL0_BASE + OVL_SRC_CON);
        fb_wait_frame_gap();
        mmio_write32(MT_DISP_OVL0_BASE + OVL_SRC_CON,
                     src | (blank_saved_src & 0xFu));
        fb_wait_frame_gap();
        backlight_set(1);
        blanked = 0;
        kprintf("ЭКРАН    : ВЕРНУЛСЯ\n");
    }
}

int ovl_blanked(void)
{
    return blanked;
}

/*
 * Что оверлей делает прямо сейчас.
 *
 * Системные полосы живут в нулевом слое: рабочую область целиком
 * закрывает окно оболочки на своём слое, и кадр виден только в двух
 * узких щелях сверху и снизу. Значит, если пропадают ОБЕ полосы разом,
 * дело не в том, кто и как рисует надписи, а в самом нулевом слое.
 *
 * Признаки прерываний читаем и тут же сбрасываем: они накапливаются
 * сами, и следующий отчёт покажет ровно то, что случилось за прошедшие
 * десять секунд. Переполнение выборки — как раз такой признак: оно
 * длится один кадр и никаким снимком регистров не ловится.
 */
void ovl_report(void)
{
    static u32 told, was_src;
    u32 src = mmio_read32(MT_DISP_OVL0_BASE + OVL_SRC_CON);

    /*
     * Полный отчёт — первые три раза и потом только когда что-то
     * изменилось. Кольцо консоли шестнадцать килобайт, и подробность
     * раз в десять секунд вытеснила бы из него всю загрузку.
     */
    if (told >= 3 && src == was_src && !fb_ovl_seen)
        return;
    told++;
    was_src = src;

    kprintf("OVL      : SRC_CON %08x ROI %08x STA %08x INTSTA %08x\n",
            mmio_read32(MT_DISP_OVL0_BASE + OVL_SRC_CON),
            mmio_read32(MT_DISP_OVL0_BASE + OVL_ROI_SIZE),
            mmio_read32(MT_DISP_OVL0_BASE + OVL_STA),
            mmio_read32(MT_DISP_OVL0_BASE + OVL_INTSTA));

    for (u32 i = 0; i < OVL_LAYERS; i++)
        kprintf("         СЛОЙ %u: CON %08x РАЗМ %08x СМЕЩ %08x АДРЕС %08x ЧТЕНИЕ %u\n",
                i,
                mmio_read32(lreg(i, OVL_L0_CON)),
                mmio_read32(lreg(i, OVL_L0_SRC_SIZE)),
                mmio_read32(lreg(i, OVL_L0_OFFSET)),
                mmio_read32(lreg(i, OVL_L0_ADDR)),
                mmio_read32(lreg(i, OVL_RDMA0_CTRL)) & 1);

    kprintf("         НАКОПЛЕНО ПРИЗНАКОВ %08x\n", fb_ovl_seen);
    fb_ovl_seen = 0;
    mmio_write32(MT_DISP_OVL0_BASE + OVL_INTSTA, 0);
}

void ovl_layer_off(u32 layer)
{
    if (layer == 0 || layer >= OVL_LAYERS)
        return;
    ovl_enable(layer, 0);
}

#else   /* в эмуляторе слоёв нет: там кадр один и складывать нечего */

void ovl_dump(void) { }
void ovl_report(void) { }
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
void ovl_layer_addr(u32 l, const volatile void *b) { (void)l; (void)b; }
void ovl_layer_alpha(u32 l, u32 a) { (void)l; (void)a; }
void ovl_layer_off(u32 l) { (void)l; }
void ovl_base_layer(int on) { (void)on; }
void ovl_take_over(void) { }

#endif
