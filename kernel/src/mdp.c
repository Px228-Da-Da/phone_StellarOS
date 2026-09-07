/*
 * Двумерный движок: разведка по затворам тактов.
 *
 * Первая разведка читала блоки подряд и повисла — ровно там, где и
 * предсказывалось: на disp_wdma0. Телефон не умер, но задача пульса
 * встала секунд на сорок (в логе пропали TICK 20, 30 и 40 и вернулся
 * TICK 50), а вывод в это время побился.
 *
 * Так и должно было быть. Обращение к блоку без тактов вешает шину, и
 * именно поэтому перед каждым чтением печаталось, куда мы идём. Догадка
 * подтвердилась поимённо.
 *
 * Теперь угадывать не надо. Раскладка затворов взята из исходника
 * вендора для этого самого чипа — out/ref/clk-mt6768.c, таблица
 * mm_clks[]. Там же и раскладка регистров:
 *
 *     static const struct mtk_gate_regs mm_cg_regs = {
 *             .set_ofs = 0x104,   поставить затвор (выключить такты)
 *             .clr_ofs = 0x108,   снять затвор (включить такты)
 *             .sta_ofs = 0x100,   состояние
 *     };
 *
 * Единица в состоянии = такты перекрыты, блок спит. Проверяется это на
 * нашем же телефоне: бит 7 (disp_ovl0) стоит в нуле, и оверлей
 * действительно работает — прямо сейчас показывает наш кадр.
 *
 * Регистра CG_CON1 у блока мультимедиа нет вовсе: в драйвере вендора
 * для mmsys описан один-единственный набор затворов. То, что первая
 * разведка прочитала по 0x110 и назвала «CON1», — что-то другое, и в
 * выводе этого больше нет, чтобы не выдавать случайное число за смысл.
 */
#include "mdp.h"
#include "io.h"
#include "print.h"
#include "usb.h"

#if defined(BOARD_MERLIN)

/* Адреса блоков — из дерева устройства этого телефона (out/stock.dts) */
#define MMSYS_BASE      0x14000000UL
#define MDP_RDMA0       0x14004000UL
#define MDP_RSZ0        0x14006000UL
#define MDP_RSZ1        0x14007000UL
#define MDP_WDMA0       0x14008000UL
#define MDP_WROT0       0x14009000UL
#define DISP_RDMA0      0x1400D000UL
#define DISP_WDMA0      0x1400E000UL
#define DISP_RSZ0       0x14015000UL

/* Затворы тактов блока мультимедиа (mm_cg_regs вендора) */
#define MMSYS_CG_CON0   0x100
#define MMSYS_CG_SET0   0x104
#define MMSYS_CG_CLR0   0x108

/*
 * Номера затворов — из таблицы mm_clks[] вендора, дословно.
 *
 * Держим их здесь все, а не только нужные: по ним читается состояние
 * всего блока, и видно, что ещё спит рядом.
 */
static const char *const gate_name[32] = {
    "mdp_rdma0",   "mdp_ccorr0",  "mdp_rsz0",    "mdp_rsz1",
    "mdp_tdshp0",  "mdp_wrot0",   "mdp_wdma0",   "disp_ovl0",
    "disp_ovl0_2l","disp_rsz0",   "disp_rdma0",  "disp_wdma0",
    "disp_color0", "disp_ccorr0", "disp_aal0",   "disp_gamma0",
    "disp_dither0","dsi0",        "fake_eng",    "smi_common",
    "smi_larb0",   "smi_comm0",   "smi_comm1",   "cam_mdp",
    "smi_img",     "smi_cam",     "smi_venc",    "smi_vdec",
    "img_dl_relay","imgdl_async", "dig_dsi",     "hrtwt",
};

/* Блок движка: где лежит и какой затвор его питает */
struct block {
    const char *name;
    u64         base;
    int         gate;
};

/*
 * Цепочка размытия: прочитать из памяти, уменьшить, записать обратно.
 * Больше нам ничего и не нужно, поэтому будим только это.
 */
static const struct block chain[] = {
    { "mdp_rdma0", MDP_RDMA0, 0 },
    { "mdp_rsz0",  MDP_RSZ0,  2 },
    { "mdp_wdma0", MDP_WDMA0, 6 },
};

/* Остальное только смотрим, если оно и так не спит */
static const struct block nearby[] = {
    { "disp_rdma0", DISP_RDMA0, 10 },
    { "disp_wdma0", DISP_WDMA0, 11 },
    { "disp_rsz0",  DISP_RSZ0,   9 },
    { "mdp_rsz1",   MDP_RSZ1,    3 },
    { "mdp_wrot0",  MDP_WROT0,   5 },
};

static u32 gates(void)
{
    return mmio_read32(MMSYS_BASE + MMSYS_CG_CON0);
}

/* Прочитать четыре слова блока. Вызывать только когда такты поданы. */
static void peek(const struct block *b)
{
    u32 v[4];

    kprintf("MDP      : ЧИТАЮ %s ПО 0x%08lx...\n", b->name, b->base);
    usb_flush();

    v[0] = mmio_read32(b->base + 0x00);
    v[1] = mmio_read32(b->base + 0x04);
    v[2] = mmio_read32(b->base + 0x08);
    v[3] = mmio_read32(b->base + 0x0C);

    kprintf("MDP      : %s: %08x %08x %08x %08x\n",
            b->name, v[0], v[1], v[2], v[3]);
    usb_flush();
}

/* Прочитать, но только если блок не спит */
static void peek_if_awake(const struct block *b, u32 cg)
{
    if (cg & (1U << b->gate)) {
        kprintf("MDP      : %s СПИТ (затвор %d), НЕ ТРОГАЮ\n",
                b->name, b->gate);
        usb_flush();
        return;
    }
    peek(b);
}

void mdp_probe(void)
{
    static int told;
    u32 cg, want;

    if (told)
        return;
    told = 1;

    cg = gates();

    kprintf("MDP      : ЗАТВОРЫ ТАКТОВ: %08x (1 = СПИТ)\n", cg);
    for (int i = 0; i < 32; i++)
        if (!(cg & (1U << i)))
            kprintf("MDP      :   ЖИВ  %2d %s\n", i, gate_name[i]);
    usb_flush();

    /*
     * Сверка, ради которой всё и затевалось: оверлей у нас работает,
     * значит его затвор обязан стоять в нуле. Если нет — раскладка
     * прочитана неправильно, и будить по ней ничего нельзя.
     */
    if (cg & (1U << 7)) {
        kprintf("MDP      : СВЕРКА НЕ ПРОШЛА: disp_ovl0 ЧИСЛИТСЯ СПЯЩИМ,\n");
        kprintf("MDP      : А ОН ПОКАЗЫВАЕТ КАДР. РАСКЛАДКА НЕВЕРНА, СТОЮ.\n");
        usb_flush();
        return;
    }
    kprintf("MDP      : СВЕРКА: disp_ovl0 ЖИВ, КАК И ДОЛЖЕН. РАСКЛАДКА ВЕРНА.\n");
    usb_flush();

    /* Что рядом — смотрим, не будя */
    for (unsigned i = 0; i < sizeof(nearby) / sizeof(nearby[0]); i++)
        peek_if_awake(&nearby[i], cg);

    /*
     * Будим цепочку размытия.
     *
     * Это первая запись за всю разведку, и она самая безобидная из
     * возможных: подать блоку такты. Питание у него уже есть — оно
     * общее с оверлеем, который работает. Разбуженный блок ничего не
     * делает сам, он просто перестаёт быть мёртвым для чтения.
     */
    want = 0;
    for (unsigned i = 0; i < sizeof(chain) / sizeof(chain[0]); i++)
        want |= 1U << chain[i].gate;

    kprintf("MDP      : БУЖУ ЦЕПОЧКУ, СНИМАЮ ЗАТВОРЫ %08x\n", want);
    usb_flush();
    mmio_write32(MMSYS_BASE + MMSYS_CG_CLR0, want);

    cg = gates();
    kprintf("MDP      : ЗАТВОРЫ СТАЛИ: %08x\n", cg);
    usb_flush();

    /*
     * Читаем только то, что действительно проснулось. Если затвор не
     * снялся, блок остался мёртвым, и лезть в него — снова повесить
     * шину на сорок секунд.
     */
    for (unsigned i = 0; i < sizeof(chain) / sizeof(chain[0]); i++)
        peek_if_awake(&chain[i], cg);

    kprintf("MDP      : РАЗВЕДКА ЗАКОНЧЕНА\n");
    usb_flush();
}

#else

void mdp_probe(void) { }

#endif
