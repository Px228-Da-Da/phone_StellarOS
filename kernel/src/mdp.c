/*
 * Двумерный движок: разведка и проверка управления.
 *
 * Что уже установлено на телефоне.
 *
 *   1. Раскладка затворов тактов верна. Прочитано 2f85fa7f; по таблице
 *      mm_clks[] вендора это значит, что живы оверлей, disp_rdma0 и
 *      dsi0, а вся двумерная цепочка спит. Сходится с наблюдаемым:
 *      оверлей действительно показывает наш кадр.
 *
 *   2. Спящий блок вешает шину. Первая разведка читала подряд и встала
 *      на disp_wdma0 — том самом, чей затвор стоит единицей. Задача
 *      пульса пропала на сорок секунд (в логе нет TICK 20, 30 и 40).
 *
 *   3. Затворами мы управляем. Сняли три (00000045) — стало 2f85fa3a,
 *      изменились ровно они и ничего больше. Разбуженные mdp_rdma0,
 *      mdp_rsz0 и mdp_wdma0 отвечают осмысленными значениями.
 *
 * Источники в out/ref/, там же в README сверка смещений. Ни одного
 * числа отсюда не выдумано.
 *
 * Поправка к прежнему комментарию: регистр 0x110 существует, вендор
 * называет его «MMSYS Clock Gating Config_1» (cmdq_mdp.c, таблица
 * configRegisters). Ошибка была в другом: драйвер тактов не заводит в
 * нём ни одного затвора, поэтому расшифровывать его по mm_clks[]
 * нельзя. Печатаем как есть, без имён.
 */
#include "mdp.h"
#include "io.h"
#include "print.h"
#include "usb.h"

#if defined(BOARD_MERLIN)

/* Адреса блоков — mdp_base-mt6768.h вендора, для этого телефона */
#define MMSYS_BASE      0x14000000UL
#define MM_MUTEX_BASE   0x14001000UL
#define MDP_RDMA0       0x14004000UL
#define MDP_RSZ0        0x14006000UL
#define MDP_RSZ1        0x14007000UL
#define MDP_WDMA0       0x14008000UL
#define MDP_WROT0       0x14009000UL
#define DISP_RDMA0      0x1400D000UL
#define DISP_WDMA0      0x1400E000UL
#define DISP_RSZ0       0x14015000UL

/* Затворы тактов (mm_cg_regs вендора: состояние, поставить, снять) */
#define MMSYS_CG_CON0   0x100
#define MMSYS_CG_SET0   0x104
#define MMSYS_CG_CLR0   0x108

/*
 * Сброс блоков движка (cmdq_mdp_reset_with_mmsys).
 *
 * Ноль в бите = блок в сбросе, единица = работает. Номера битов те же,
 * что у затворов тактов: rdma0 = 0, rsz0 = 2, wdma = 6. Это два разных
 * файла вендора, сошедшихся на одной нумерации, — лишнее подтверждение,
 * что таблица прочитана правильно.
 */
#define MMSYS_SW0_RST_B 0x140

/* Разводка: кто кому отдаёт кадр (configRegisters вендора) */
#define MMSYS_MDP_RDMA0_MOUT_EN 0xF08
#define MMSYS_MDP_PRZ0_MOUT_EN  0xF10
#define MMSYS_MDP_PRZ0_SEL_IN   0xF24
#define MMSYS_MDP_WDMA_SEL_IN   0xF34
#define MMSYS_MDP_WROT0_SEL_IN  0xF38
#define MMSYS_MDP_DL_VALID_0    0xFB0
#define MMSYS_MDP_DL_READY_0    0xFC0

/* Номера затворов из mm_clks[] вендора, дословно */
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

struct block {
    const char *name;
    u64         base;
    int         gate;       /* он же номер бита сброса */
    u32         probe;      /* безобидный регистр настройки для пробы */
    const char *probe_name;
};

/*
 * Цепочка размытия: прочитать из памяти, уменьшить, записать обратно.
 *
 * У каждого блока выбран один регистр, в который можно написать и не
 * получить последствий: он задаёт размер картинки, а блоки выключены
 * (бит разрешения у всех троих в нуле — видно по прочитанному). Смещения
 * из mdp3, сверка в docs/09.
 */
static const struct block chain[] = {
    { "mdp_rdma0", MDP_RDMA0, 0, 0x070, "MF_SRC_SIZE " },
    { "mdp_rsz0 ", MDP_RSZ0,  2, 0x010, "INPUT_IMAGE " },
    { "mdp_wdma0", MDP_WDMA0, 6, 0x018, "SRC_SIZE    " },
};
#define CHAIN_N (sizeof(chain) / sizeof(chain[0]))

/*
 * Мультиплексоры разводки, все подряд и по порядку.
 *
 * Список и имена — из configRegisters вендора для MT6768. Смещения идут
 * сплошняком через четыре байта: сначала «кому отдавать» (F04..F1C),
 * потом «у кого брать» (F20..F38). Набор беднее, чем у MT8183: ни AAL,
 * ни IPU, ни PATH0/PATH1 — у вендора они закомментированы.
 */
struct mux_reg {
    const char *name;
    u32         off;
};

static const struct mux_reg mux[] = {
    { "ISP_MOUT_EN      ", 0xF04 },
    { "RDMA0_MOUT_EN    ", 0xF08 },
    { "CCORR_MOUT_EN    ", 0xF0C },
    { "PRZ0_MOUT_EN     ", 0xF10 },
    { "PRZ1_MOUT_EN     ", 0xF14 },
    { "TDSHP_SOUT_SEL   ", 0xF18 },
    { "COLOR_MOUT_EN    ", 0xF1C },
    { "CCORR_SEL_IN     ", 0xF20 },
    { "PRZ0_SEL_IN      ", 0xF24 },
    { "PRZ1_SEL_IN      ", 0xF28 },
    { "TDSHP_SEL_IN     ", 0xF2C },
    { "COLOR_OUT_SEL_IN ", 0xF30 },
    { "WDMA_SEL_IN      ", 0xF34 },
    { "WROT0_SEL_IN     ", 0xF38 },
};
#define MUX_N (sizeof(mux) / sizeof(mux[0]))

static u32 gates(void)
{
    return mmio_read32(MMSYS_BASE + MMSYS_CG_CON0);
}

/*
 * Имена выровнены пробелами прямо в тексте, а не шириной поля.
 *
 * Наш kprintf разбирает у формата только ноль и ширину, а флаг «влево»
 * не понимает вовсе: "%-18s" он печатает буквально и на этом сбивается
 * со счёта аргументов. Один раз уже сбился — весь отчёт о разводке
 * вышел мусором: вместо значений печатались указатели на имена.
 */
static void show(const char *name, u32 off)
{
    kprintf("MDP      :   %s (%03x) %08x\n",
            name, off, mmio_read32(MMSYS_BASE + off));
}

/* Прочитать четыре слова блока. Вызывать только когда такты поданы. */
static void peek(const struct block *b)
{
    u32 v[4];

    v[0] = mmio_read32(b->base + 0x00);
    v[1] = mmio_read32(b->base + 0x04);
    v[2] = mmio_read32(b->base + 0x08);
    v[3] = mmio_read32(b->base + 0x0C);

    kprintf("MDP      :   %s %08x %08x %08x %08x\n",
            b->name, v[0], v[1], v[2], v[3]);
}

/*
 * Разводка экранной половины. Здесь она вся, поимённо.
 *
 * Взято из драйвера экрана вендора для этого самого чипа:
 * drivers/misc/mediatek/video/mt6768/dispsys/ — смещения в
 * ddp_reg_mmsys.h, а кто с кем соединяется — в ddp_path.c, таблицы
 * mout_map[] и sel_in_map[]. Не с соседнего чипа и не по соглашению:
 * ровно MT6768.
 *
 * Это то, чего не хватало движку. У двумерной половины значения
 * мультиплексоров нигде не опубликованы, а у экранной — опубликованы
 * целиком, и в ней есть ровно то, что нам нужно:
 *
 *     DISP_OVL0_MOUT_EN бит 2  → WDMA0
 *     DISP_WDMA0_SEL_IN = 1    → берёт у OVL0
 *
 * То есть снимок того, что оверлей уже собрал, прямо в память. А
 * MOUT_EN — маска, а не выбор: бит на RDMA0 можно оставить, и экран
 * продолжит показывать, пока мы снимаем. Через RSZ0 (у него бит 4 на
 * WDMA0) снимок можно сразу и уменьшить — это и есть заготовка для
 * размытия, посчитанная железом.
 *
 * Но сначала читаем. Если карта верна, она обязана описать ту цепочку,
 * которую построил загрузчик и которая прямо сейчас показывает картинку.
 * Не опишет — значит понята неправильно, и трогать по ней ничего нельзя.
 */
static const char *const to_ovl0[]    = { "rdma0", "ovl0_2l", "wdma0", "rsz0", 0 };
static const char *const to_ovl0_2l[] = { "rdma0", "wdma0", "rsz0", 0 };
static const char *const to_rsz0[]    = { "rdma0", "ovl0", "ovl0_2l", "rsz0_virt1", "wdma0", 0 };
static const char *const to_dither0[] = { "dsi0", "wdma0", 0 };

static const char *const in_rdma0[]   = { "ovl0", "ovl0_2l", "rsz0", 0 };
static const char *const in_rsz0[]    = { "ovl0", "ovl0_2l", "rdma0", 0 };
static const char *const in_rsz0v1[]  = { "rsz0_virt0", "rsz0", 0 };
static const char *const in_ccorr0[]  = { "color0", "rsz0_virt1", 0 };
static const char *const in_dsi0[]    = { "rsz0_virt1", "dither0", 0 };
static const char *const in_wdma0[]   = { "dither0", "ovl0", "ovl0_2l", "rsz0", 0 };
static const char *const out_rdma0[]  = { "dsi0", "color0", "ccorr0", 0 };
static const char *const out_rd_rsz[] = { "rsz0_virt0", "rsz0", 0 };

struct route {
    const char         *name;
    u32                 off;
    int                 is_mask;    /* 1 = маска получателей, 0 = номер источника */
    const char *const  *who;
};

static const struct route routes[] = {
    { "OVL0_MOUT_EN    ", 0xF3C, 1, to_ovl0    },
    { "OVL0_2L_MOUT_EN ", 0xF40, 1, to_ovl0_2l },
    { "RSZ0_MOUT_EN    ", 0xF44, 1, to_rsz0    },
    { "DITHER0_MOUT_EN ", 0xF50, 1, to_dither0 },
    { "RDMA0_RSZ0_SOUT ", 0xF48, 0, out_rd_rsz },
    { "RDMA0_SOUT_SEL  ", 0xF4C, 0, out_rdma0  },
    { "PATH0_SEL_IN    ", 0xF54, 0, in_rdma0   },
    { "RSZ0_SEL_IN     ", 0xF58, 0, in_rsz0    },
    { "RDMA0_RSZ0_SELIN", 0xF60, 0, in_rsz0v1  },
    { "COLOR0_OUT_SELIN", 0xF64, 0, in_ccorr0  },
    { "DSI0_SEL_IN     ", 0xF68, 0, in_dsi0    },
    { "WDMA0_SEL_IN    ", 0xF6C, 0, in_wdma0   },
};
#define ROUTE_N (sizeof(routes) / sizeof(routes[0]))

static void disp_routes(void)
{
    kprintf("MDP      : РАЗВОДКА ЭКРАНА (ПО КАРТЕ ВЕНДОРА ДЛЯ MT6768):\n");

    for (unsigned i = 0; i < ROUTE_N; i++) {
        u32 v = mmio_read32(MMSYS_BASE + routes[i].off);

        if (routes[i].is_mask) {
            kprintf("MDP      :   %s (%03x) %08x ОТДАЁТ:\n",
                    routes[i].name, routes[i].off, v);
            for (unsigned b = 0; routes[i].who[b]; b++)
                if (v & (1U << b))
                    kprintf("MDP      :       %s\n", routes[i].who[b]);
            if (!v)
                kprintf("MDP      :       НИКОМУ\n");
        } else {
            unsigned n = 0;

            while (routes[i].who[n])
                n++;
            kprintf("MDP      :   %s (%03x) %08x БЕРЁТ У %s\n",
                    routes[i].name, routes[i].off, v,
                    v < n ? routes[i].who[v] : "??? (ВНЕ КАРТЫ)");
        }
    }

    /*
     * Сверка с работающим экраном.
     *
     * Картинку сейчас показывает цепочка, построенная загрузчиком, и
     * начинается она с оверлея — это мы знаем твёрдо, потому что сами
     * пишем в его слои и видим результат. Значит у OVL0 обязан стоять
     * хоть один бит «кому отдавать». Не стоит — карта прочитана
     * неправильно, и по ней нельзя ни снимать экран, ни что-то менять.
     */
    if (mmio_read32(MMSYS_BASE + 0xF3C))
        kprintf("MDP      : СВЕРКА: OVL0 КОМУ-ТО ОТДАЁТ, КАК И ДОЛЖЕН\n");
    else
        kprintf("MDP      : СВЕРКА НЕ ПРОШЛА: OVL0 НИКОМУ НЕ ОТДАЁТ,\n"
                "MDP      : А КАРТИНКА ИДЁТ. КАРТА НЕВЕРНА, ПО НЕЙ НЕ ДЕЙСТВУЮ.\n");
    usb_flush();
}

void mdp_probe(void)
{
    static int told;
    u32 cg, want, rst, held, got;
    u32 was[CHAIN_N];

    if (told)
        return;
    told = 1;

    disp_routes();

    cg = gates();
    kprintf("MDP      : ЗАТВОРЫ ТАКТОВ %08x (1 = СПИТ)\n", cg);
    for (int i = 0; i < 32; i++)
        if (!(cg & (1U << i)))
            kprintf("MDP      :   ЖИВ %2d %s\n", i, gate_name[i]);

    /*
     * Сверка перед любым действием: оверлей показывает кадр прямо
     * сейчас, значит его затвор обязан читаться нулём. Не читается —
     * таблица понята неправильно, и по ней ничего нельзя ни будить, ни
     * сбрасывать.
     */
    if (cg & (1U << 7)) {
        kprintf("MDP      : СВЕРКА НЕ ПРОШЛА: disp_ovl0 ЧИСЛИТСЯ СПЯЩИМ,\n");
        kprintf("MDP      : А ОН ПОКАЗЫВАЕТ КАДР. ТАБЛИЦА НЕВЕРНА, СТОЮ.\n");
        usb_flush();
        return;
    }

    /* Будим цепочку, если она ещё спит */
    want = 0;
    for (unsigned i = 0; i < CHAIN_N; i++)
        want |= 1U << chain[i].gate;

    if (cg & want) {
        mmio_write32(MMSYS_BASE + MMSYS_CG_CLR0, want);
        cg = gates();
        kprintf("MDP      : РАЗБУДИЛ ЦЕПОЧКУ, ЗАТВОРЫ СТАЛИ %08x\n", cg);
    }
    if (cg & want) {
        kprintf("MDP      : ЦЕПОЧКА НЕ ПРОСНУЛАСЬ, ДАЛЬШЕ НЕ ИДУ\n");
        usb_flush();
        return;
    }
    usb_flush();

    /*
     * Разводка. Это то, чего нам не хватает, чтобы соединить блоки в
     * цепочку: какие регистры сейчас говорят, кто кому отдаёт кадр.
     * Пока только читаем — надо знать, от чего отталкиваться.
     */
    kprintf("MDP      : РАЗВОДКА В MMSYS СЕЙЧАС:\n");
    show("CG_CON0       ", MMSYS_CG_CON0);
    show("CG_CON1       ", 0x110);
    show("SW0_RST_B     ", MMSYS_SW0_RST_B);
    show("RDMA0_MOUT_EN ", MMSYS_MDP_RDMA0_MOUT_EN);
    show("PRZ0_MOUT_EN  ", MMSYS_MDP_PRZ0_MOUT_EN);
    show("PRZ0_SEL_IN   ", MMSYS_MDP_PRZ0_SEL_IN);
    show("WDMA_SEL_IN   ", MMSYS_MDP_WDMA_SEL_IN);
    show("WROT0_SEL_IN  ", MMSYS_MDP_WROT0_SEL_IN);
    show("DL_VALID_0    ", MMSYS_MDP_DL_VALID_0);
    show("DL_READY_0    ", MMSYS_MDP_DL_READY_0);
    usb_flush();

    /*
     * Ширина мультиплексоров.
     *
     * Соединить блоки в цепочку мешает последнее незнание: чем именно
     * записывается «rdma0 отдаёт кадр вот этому». По соседнему чипу
     * (MT8183, патч в ядро «soc: mediatek: mmsys: Add support for MDP»)
     * видно соглашение: MOUT_EN — маска, по биту на получателя; SEL_IN —
     * просто номер источника. Но у MT8183 и смещения другие, и набор
     * блоков богаче, так что переносить оттуда сами числа было бы
     * гаданием.
     *
     * Зато ширину можно измерить. Пишем во все разряды единицы и читаем
     * обратно: железо оставит только те, которые у него есть. Сколько
     * бит вернулось у MOUT_EN — столько у блока получателей; какое
     * наибольшее число вернулось у SEL_IN — столько у него источников.
     * Это уже не догадка, а замер, и он режет перебор до считаных
     * вариантов.
     *
     * Безопасно: блоки выключены и ничего не передают, а прежние
     * значения кладём обратно и печатаем, что положили.
     */
    kprintf("MDP      : ШИРИНА МУЛЬТИПЛЕКСОРОВ:\n");
    for (unsigned i = 0; i < MUX_N; i++) {
        u32 keep = mmio_read32(MMSYS_BASE + mux[i].off);

        mmio_write32(MMSYS_BASE + mux[i].off, 0xFFFFFFFF);
        got = mmio_read32(MMSYS_BASE + mux[i].off);
        mmio_write32(MMSYS_BASE + mux[i].off, keep);

        kprintf("MDP      :   %s (%03x) РАЗРЯДЫ %08x, ВЕРНУЛ %08x\n",
                mux[i].name, mux[i].off, got,
                mmio_read32(MMSYS_BASE + mux[i].off));
    }
    usb_flush();

    kprintf("MDP      : ПЕРВЫЕ ЧЕТЫРЕ СЛОВА КАЖДОГО БЛОКА:\n");
    for (unsigned i = 0; i < CHAIN_N; i++)
        peek(&chain[i]);
    usb_flush();

    /*
     * Управляем ли мы блоками или только смотрим на них.
     *
     * Прошлый раз это проверялось сбросом: сбросить и посмотреть, не
     * изменились ли регистры. Ответ вышел бессмысленный — они не
     * изменились, но это ничего не доказывает: сброс здесь гасит
     * состояние блока, а настройки в нём вполне могут пережить. Так что
     * непонятно было, то ли сброс не сработал, то ли сработал и так и
     * должно быть.
     *
     * Поэтому теперь проверка прямая: пишем в регистр настройки своё
     * число и читаем обратно. Прочиталось — блок наш, и никаких
     * толкований это не допускает.
     *
     * Число 00400020 выбрано читаемым в логе, а не круглым: сплошные
     * нули или единицы можно спутать с тем, что регистр просто не
     * отвечает.
     */
    kprintf("MDP      : ПРОБА ЗАПИСИ (ПИШУ 00400020, ЧИТАЮ ОБРАТНО):\n");
    for (unsigned i = 0; i < CHAIN_N; i++) {
        was[i] = mmio_read32(chain[i].base + chain[i].probe);
        mmio_write32(chain[i].base + chain[i].probe, 0x00400020);
        got = mmio_read32(chain[i].base + chain[i].probe);
        kprintf("MDP      :   %s %s БЫЛО %08x СТАЛО %08x %s\n",
                chain[i].name, chain[i].probe_name, was[i], got,
                got == 0x00400020 ? "— ПИШЕТСЯ" : "— НЕ ПИШЕТСЯ");
    }
    usb_flush();

    /*
     * Сброс. Теперь он проверяет сам себя: в регистрах лежит наше
     * число, и если сброс их обнулит — значит он дошёл до блока.
     * Останется как было — значит настройки сброс переживают, что тоже
     * знание, но уже без гаданий.
     *
     * Вендор пишет регистр целиком: сначала ~наши_биты, потом ~0. Второе
     * слово выводит из сброса вообще всё, включая экранные блоки. Нам
     * это не нужно: читаем, гасим только свои три бита, возвращаем
     * прочитанное. Между двумя записями читаем регистр обратно — заодно
     * видно, доходит ли до него запись вообще.
     */
    held = mmio_read32(MMSYS_BASE + MMSYS_SW0_RST_B);
    rst = 0;
    for (unsigned i = 0; i < CHAIN_N; i++)
        rst |= 1U << chain[i].gate;

    mmio_write32(MMSYS_BASE + MMSYS_SW0_RST_B, held & ~rst);
    kprintf("MDP      : СБРОС: БЫЛО %08x, ПРОСИЛ %08x, В РЕГИСТРЕ %08x\n",
            held, held & ~rst, mmio_read32(MMSYS_BASE + MMSYS_SW0_RST_B));
    mmio_write32(MMSYS_BASE + MMSYS_SW0_RST_B, held);
    usb_flush();

    kprintf("MDP      : ПОСЛЕ СБРОСА (00400020 = ПЕРЕЖИЛО, 0 = СТЁРТО):\n");
    for (unsigned i = 0; i < CHAIN_N; i++) {
        got = mmio_read32(chain[i].base + chain[i].probe);
        kprintf("MDP      :   %s %s %08x\n",
                chain[i].name, chain[i].probe_name, got);
        /* Возвращаем как было: разведка не оставляет следов */
        mmio_write32(chain[i].base + chain[i].probe, was[i]);
    }

    kprintf("MDP      : РАЗВЕДКА ЗАКОНЧЕНА, ЭКРАН НЕ ТРОНУТ\n");
    usb_flush();
}

#else

void mdp_probe(void) { }

#endif
