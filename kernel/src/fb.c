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
#include "pmm.h"
#include "mmu.h"
#include "print.h"

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
    volatile u32 *base;                 /* куда рисуем прямо сейчас */
    volatile u32 *buf[2];               /* оба кадровых буфера       */
    u32 draw;                           /* индекс рисуемого          */
    u32 vram_size;                      /* сколько видеопамяти отдал загрузчик */
    int double_buffered;
    u32 width, height;
    u32 stride_px;                      /* пикселей в строке (может быть > width) */
    u32 fg, bg;
    u32 cur_x, cur_y;                   /* курсор консоли в пикселях */
    u32 scale;                          /* во сколько раз растянут глиф 8x8 */
    int ready;
} fb;

/* Чем именно нашли буфер — видно снаружи через диагностику:
 * 0 не нашли, 1 из device tree, 2 из регистра оверлея. */
/* Сколько раз дождались конца кадра и сколько раз не дождались —
 * чтобы не гадать, работает ли синхронизация вообще. */
u32 fb_vsync_hits, fb_vsync_misses;

int fb_probe_source;
u32 fb_lcm_inited;                      /* включил ли LK саму панель */

static void fb_present(void);           /* платформенная подмена буфера */

/* Размер одного кадра в байтах */
static u64 fb_frame_bytes(void)
{
    return (u64)fb.stride_px * 4 * fb.height;
}

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

    /* Сколько видеопамяти отдал загрузчик: от этого зависит, поместится ли
     * второй кадр рядом с первым или придётся просить память у аллокатора. */
    fb.vram_size = 0;
    if (fdt_root())
        fdt_node_prop_u32(fdt_root(), "chosen", "atag,videolfb-vramSize",
                          &fb.vram_size);

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

/*
 * Настоящий page flip: контроллер дисплея начнёт выводить другой буфер.
 * Адрес слоя мы из этого регистра читаем — значит можем и записать.
 */
/*
 * Дождаться события от оверлея.
 *
 * Снимаем защёлку и ждём следующее. Срок — два кадра: если признак не
 * пришёл за это время, значит ждём не того, и лучше показать шов, чем
 * повесить ядро.
 */
static int fb_wait_ovl(u32 flag)
{
    u64 deadline = read_cntpct() + read_cntfrq() / 30;   /* два кадра */

    mmio_write32(MT_DISP_OVL0_BASE + OVL_INTSTA, 0);
    dsb();

    while (read_cntpct() < deadline)
        if (mmio_read32(MT_DISP_OVL0_BASE + OVL_INTSTA) & flag)
            return 1;
    return 0;
}

/*
 * Подменить показываемый буфер.
 *
 * Раньше ждали конца кадра у RDMA — и это было ошибкой, хотя признак
 * ловился исправно, 92 раза из 92. Цепочка вывода такая:
 *
 *     OVL  ->  RDMA  ->  ...  ->  панель
 *   читает    отдаёт
 *   память    панели
 *
 * Из нашей памяти читает OVL, а не RDMA. Когда RDMA досылает кадр панели,
 * OVL давно начал читать следующий — по старому ещё адресу. Мы в этот
 * момент считали буфер свободным и начинали в него рисовать. В итоге в
 * один кадр попадали куски двух разных, и на движущейся картинке это
 * выглядело как множество горизонтальных полос.
 *
 * Поэтому ждём OVL и делаем два ожидания, а не одно:
 *
 *   дочитал кадр  -> буфер свободен, можно подменять адрес
 *   начал кадр    -> новый адрес вступил в силу, старый буфер свободен
 *
 * Второе ожидание и есть то, чего не хватало: без него мы возвращались
 * рисовать в буфер, который всё ещё читают.
 *
 * Замер на устройстве: все четыре признака приходят ровно 60 раз в
 * секунду, а занят OVL 99.5% времени — окно на подмену около 85 мкс.
 */
int fb_wait_frame_gap(void)
{
    return fb_wait_ovl(OVL_INT_FRAME_CPL);
}

/*
 * Идёт ли прямо сейчас чтение кадра из памяти.
 *
 * Дождаться окна мало: оно длится около восьмидесяти пяти микросекунд, а
 * между «окно наступило» и «регистр записан» задачу может вытеснить
 * прерывание таймера. Поэтому перед самой записью спрашиваем оверлей
 * ещё раз — этот бит показывает состояние, а не событие, и врать ему
 * незачем.
 */
int fb_frame_idle(void)
{
    return !(mmio_read32(MT_DISP_OVL0_BASE + OVL_STA) & OVL_STA_RUN);
}

static void fb_present(void)
{
    if (fb_wait_ovl(OVL_INT_FRAME_CPL))
        fb_vsync_hits++;
    else
        fb_vsync_misses++;

    mmio_write32(MT_DISP_OVL0_BASE + OVL_L0_ADDR,
                 (u32)(uintptr_t)fb.buf[fb.draw]);
    dsb();

    /* Пока OVL не начал читать заново, старый буфер ещё под ним */
    fb_wait_ovl(OVL_INT_START);
}

/*
 * Сколько раз в секунду каждый блок сообщает о кадре.
 *
 * Полосы на движущейся картинке мы уже пробовали лечить ожиданием конца
 * кадра у RDMA, и признак ловился — 92 раза из 92. Значит дело не в том,
 * что синхронизации нет, а в том, что она не с тем блоком: панели кадр
 * отдаёт RDMA, а из нашей памяти читает OVL, и свободен буфер не когда
 * кадр ушёл на экран, а когда OVL его дочитал.
 *
 * Прежде чем менять — считаем. У панели 60 герц, значит верный признак
 * обязан приходить около шестидесяти раз в секунду. Всё, что заметно
 * чаще, — это что-то другое, и синхронизироваться по нему бессмысленно.
 */
static u32 fb_count_flag(u64 reg, u32 bit, u32 ms)
{
    u64 freq = read_cntfrq();
    u64 end = read_cntpct() + (freq / 1000) * ms;
    u32 count = 0;

    mmio_write32(reg, 0);
    dsb();
    while (read_cntpct() < end) {
        if (mmio_read32(reg) & bit) {
            count++;
            mmio_write32(reg, 0);       /* снять защёлку и ждать следующий */
            dsb();
        }
    }
    return count;
}

void fb_vsync_probe(void)
{
#if defined(BOARD_MERLIN)
    u32 rdma_end   = fb_count_flag(MT_DISP_RDMA0_BASE + RDMA_INT_STATUS,
                                   RDMA_INT_FRAME_END, 1000);
    u32 rdma_start = fb_count_flag(MT_DISP_RDMA0_BASE + RDMA_INT_STATUS,
                                   RDMA_INT_FRAME_START, 1000);
    u32 ovl_cpl    = fb_count_flag(MT_DISP_OVL0_BASE + OVL_INTSTA,
                                   OVL_INT_FRAME_CPL, 1000);
    u32 ovl_start  = fb_count_flag(MT_DISP_OVL0_BASE + OVL_INTSTA,
                                   OVL_INT_START, 1000);
    u32 run = 0;

    /* Заодно посмотрим, простаивает ли OVL между кадрами: если он читает
     * непрерывно, безопасного окна для подмены адреса просто нет. */
    for (u32 i = 0; i < 100000; i++)
        if (mmio_read32(MT_DISP_OVL0_BASE + OVL_STA) & OVL_STA_RUN)
            run++;

    kprintf("КАДРЫ ЗА СЕКУНДУ: RDMA КОНЕЦ %u НАЧАЛО %u, "
            "OVL ДОЧИТАЛ %u НАЧАЛ %u\n",
            rdma_end, rdma_start, ovl_cpl, ovl_start);
    kprintf("OVL ЗАНЯТ %u ИЗ 100000 ЗАМЕРОВ, INTEN %08x\n",
            run, mmio_read32(MT_DISP_OVL0_BASE + OVL_INTEN));
#endif
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
    fb.vram_size = 0;                   /* ramfb второго кадра не даёт */
    return 0;
}

/*
 * В эмуляторе адрес буфера задан один раз через fw_cfg и на лету не меняется,
 * поэтому переключение сэмулировано копированием. Смысл в том, чтобы логика
 * двойной буферизации отлаживалась здесь, а не сразу на телефоне: главное
 * правило проекта — каждая фича сперва в QEMU.
 */
/* В эмуляторе цепочки вывода нет, считать нечего */
void fb_vsync_probe(void) { }
int  fb_wait_frame_gap(void) { return 1; }
int  fb_frame_idle(void) { return 1; }

static void fb_present(void)
{
    volatile u32 *src = fb.buf[fb.draw];
    volatile u32 *dst = fb.buf[0];
    u64 words = fb_frame_bytes() / 4;

    if (src == dst)
        return;
    for (u64 i = 0; i < words; i++)
        dst[i] = src[i];
    dsb();
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

    fb.buf[0] = fb.base;                /* тот, что показывает загрузчик */
    fb.buf[1] = 0;
    fb.draw = 0;
    fb.double_buffered = 0;

    fb.ready = 1;
    return 0;
}

/*
 * Двойная буферизация.
 *
 * Зачем: сейчас мы пишем прямо в тот буфер, который контроллер дисплея в этот
 * самый момент выводит на панель. Для статичного текста незаметно, но любая
 * перерисовка кадра покажет разрыв — половина экрана новая, половина старая.
 *
 * Как: рисуем во второй буфер, а показ переключаем одной записью в регистр
 * оверлея. Адрес буфера мы оттуда читаем — значит можем и записать, и это
 * настоящий page flip, без копирования кадра.
 *
 * Где взять память под второй кадр: сначала пробуем видеопамять самого
 * загрузчика — он сообщает её размер в /chosen (atag,videolfb-vramSize) и
 * обычно резервирует место под несколько кадров как раз для этого. Если не
 * хватило, берём непрерывный блок у собственного аллокатора.
 *
 * Вызывать только после pmm_init: до него запасного пути просто нет.
 */
int fb_double_buffer(int on)
{
    u64 frame = fb_frame_bytes();

    if (!fb.ready)
        return -1;

    if (!on) {
        /* Возвращаем показ на исходный буфер загрузчика */
        fb.draw = 0;
        fb.base = fb.buf[0];
        fb_present();
        fb.double_buffered = 0;
        return 0;
    }

    if (fb.double_buffered)
        return 0;

    if (!fb.buf[1]) {
        if (fb.vram_size >= 2 * frame) {
            /* Место есть в видеопамяти загрузчика: она уже исключена из
             * учёта pmm, брать её безопаснее всего.
             *
             * Но некэшируемым в kmain помечен только ПЕРВЫЙ буфер и ровно
             * на свою длину — второй лежит сразу за ним и попадает в
             * обычную кэшируемую память. Без этой строки записи оседают
             * в кэше процессора, контроллер дисплея читает DRAM напрямую
             * и показывает старое содержимое вперемешку с новым. */
            fb.buf[1] = fb.buf[0] + fb_frame_bytes() / 4;
            mmu_set_range_nc((u64)(uintptr_t)fb.buf[1], frame);
        } else {
            /* Целыми блоками по два мегабайта: некэшируемой память
             * помечается блоками такого размера, и соседей у буфера в
             * блоке быть не должно — иначе данные ядра станут
             * некэшируемыми вместе с ним. */
            u64 span = (frame + 2UL * 1024 * 1024 - 1) & ~(2UL * 1024 * 1024 - 1);
            void *p = pmm_alloc_dma(frame);

            if (!p)
                return -1;
            /* Контроллер дисплея читает DRAM мимо кэшей процессора */
            mmu_set_range_nc((u64)(uintptr_t)p, span);
            fb.buf[1] = (volatile u32 *)p;
        }
    }

    /* Чтобы первый же кадр не мигнул мусором, копируем в него текущий */
    for (u64 i = 0; i < frame / 4; i++)
        fb.buf[1][i] = fb.buf[0][i];
    dsb();

    fb.draw = 1;
    fb.base = fb.buf[1];
    fb.double_buffered = 1;
    return 0;
}

/*
 * Показать нарисованное и начать рисовать в освободившийся буфер.
 *
 * Копирования нет намеренно: смысл двойной буферизации в том, чтобы каждый
 * кадр рисовался целиком заново. Дорисовывать поверх предыдущего здесь
 * нельзя — в новом буфере лежит кадр двухдавности.
 */
void fb_flip(void)
{
    if (!fb.ready)
        return;
    dsb();
    if (!fb.double_buffered)
        return;

    fb_present();                       /* показать тот, куда рисовали */
    fb.draw ^= 1;
    fb.base = fb.buf[fb.draw];
}

int fb_double_buffered(void) { return fb.double_buffered; }

/* Всё, что известно про экран, одним куском: по фотографии издалека
 * отличить настоящую картинку от смаза камеры невозможно, а цифры врать
 * не станут. */
void fb_debug(void)
{
    kprintf("FB БУФЕР0: 0x%016lx\n", (u64)(uintptr_t)fb.buf[0]);
    kprintf("FB БУФЕР1: 0x%016lx\n", (u64)(uintptr_t)fb.buf[1]);
    kprintf("FB КАДР  : %lu БАЙТ, VRAM %u БАЙТ\n",
            fb_frame_bytes(), fb.vram_size);
    kprintf("FB ГЕОМ  : %ux%u STRIDE %u PX\n", fb.width, fb.height,
            fb.stride_px);
    kprintf("FB ИСТОЧ : %d (1=DTB 2=РЕГИСТР), LCM %u\n",
            fb_probe_source, fb_lcm_inited);
    kprintf("VSYNC    : ПОЙМАН %u, ПРОПУЩЕН %u\n",
            fb_vsync_hits, fb_vsync_misses);
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
 * Текст в произвольном месте кадра.
 *
 * Консольный вывод умеет только «следующий символ в следующей позиции», и
 * для интерфейса этого мало: подпись на кнопке должна лечь туда, где эта
 * кнопка, и своим размером.
 *
 * Прозрачный фон задаётся нулевой альфой: тогда закрашиваются только
 * пиксели самих букв, а картинка под ними остаётся. Сплошной прямоугольник
 * под каждой подписью выглядел бы заплаткой.
 */
static void draw_glyph_ex(u32 cp, u32 px, u32 py, u32 scale,
                          u32 fg, u32 bg, int opaque_bg)
{
    const u8 *glyph = glyph_for(cp);

    for (u32 row = 0; row < 8; row++) {
        u8 bits = glyph[row];

        for (u32 col = 0; col < 8; col++) {
            int on = (bits & (0x80 >> col)) != 0;

            if (!on && !opaque_bg)
                continue;
            for (u32 sy = 0; sy < scale; sy++) {
                u32 y = py + row * scale + sy;

                if (y >= fb.height)
                    return;
                for (u32 sx = 0; sx < scale; sx++) {
                    u32 x = px + col * scale + sx;

                    if (x >= fb.width)
                        break;
                    fb.base[(u64)y * fb.stride_px + x] = on ? fg : bg;
                }
            }
        }
    }
}

/*
 * Разбор UTF-8 для строки.
 *
 * Тот же случай, что и в консоли: текст ядра в UTF-8, кириллица занимает
 * два байта. Здесь строка известна целиком, поэтому автомат не нужен —
 * просто читаем ведущий байт и, если он двухбайтовый, забираем второй.
 */
static u32 utf8_next(const char **s)
{
    const u8 *p = (const u8 *)*s;
    u32 b = *p++;

    if (b >= 0xC0 && b <= 0xDF && (*p & 0xC0) == 0x80) {
        u32 cp = ((b & 0x1F) << 6) | (*p & 0x3F);

        p++;
        *s = (const char *)p;
        return cp;
    }
    *s = (const char *)p;
    return b;
}

u32 fb_text_width(u32 scale, const char *s)
{
    u32 n = 0;

    if (!s || !scale)
        return 0;
    while (*s) {
        utf8_next(&s);
        n++;
    }
    return n * 8 * scale;
}

void fb_text(u32 x, u32 y, u32 scale, u32 fg, u32 bg, const char *s)
{
    int opaque_bg = (bg >> 24) != 0;

    if (!fb.ready || !s || !scale)
        return;
    while (*s) {
        u32 cp = utf8_next(&s);

        if (x + 8 * scale > fb.width)
            break;
        draw_glyph_ex(cp, x, y, scale, fg, bg, opaque_bg);
        x += 8 * scale;
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
