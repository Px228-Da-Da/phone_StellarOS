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
#include "pmm.h"
#include "sched.h"

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


static u32 gates(void)
{
    return mmio_read32(MMSYS_BASE + MMSYS_CG_CON0);
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

/*
 * Снимок экрана в память через DISP_WDMA0.
 *
 * Всё, что здесь пишется, взято из драйвера экрана вендора для MT6768
 * (out/ref/disp, разбор в docs/09). Ни одного числа наугад.
 *
 * Замысел. Оверлей уже собирает кадр и отдаёт его дальше по цепочке
 * OVL0 → OVL0_2L → RDMA0 → DSI0 — это прочитано на телефоне. Регистр
 * OVL0_MOUT_EN не выбирает получателя, а разрешает: это маска. Значит
 * можно добавить второго получателя, не отняв первого, — и WDMA0 положит
 * тот же кадр в память, пока панель продолжает его показывать.
 *
 * Риск и как он закрыт. Связь между блоками — рукопожатие: если WDMA0
 * не готов принимать, оверлей может встать в ожидании согласия, и экран
 * замрёт. Поэтому, во-первых, WDMA0 настраивается и включается ДО того,
 * как ему что-то пошлют. Во-вторых, окно риска закрывается само:
 * подключение живёт пятьдесят миллисекунд — три кадра при шестидесяти
 * герцах, — после чего маска возвращается к прежнему значению
 * безусловно, вышло что-нибудь или нет. Даже если предположение неверно,
 * экран замрёт на три кадра и оживёт.
 *
 * Снимаем не весь экран, а квадрат 256x256: этого хватает, чтобы
 * доказать, что кадр доходит, а буфер выходит на четверть мегабайта
 * вместо десяти.
 */

/* Регистры DISP_WDMA0 — ddp_reg_dma.h вендора */
#define WDMA0_BASE          0x1400E000UL
#define WDMA_INTSTA         0x004
#define WDMA_EN             0x008
#define WDMA_RST            0x00C
#define WDMA_CFG            0x014
#define WDMA_SRC_SIZE       0x018
#define WDMA_CLIP_SIZE      0x01C
#define WDMA_CLIP_COORD     0x020
#define WDMA_DST_W_IN_BYTE  0x028
#define WDMA_ALPHA          0x02C
#define WDMA_FLOW_CTRL_DBG  0x0A0
#define WDMA_DST_ADDR0      0xF00

/* Разводка экрана — ddp_reg_mmsys.h вендора */
#define DISP_OVL0_MOUT_EN   0xF3C
#define DISP_WDMA0_SEL_IN   0xF6C
#define OVL0_MOUT_TO_WDMA0  (1U << 2)   /* mout_map[0]: третий получатель */
#define WDMA0_SEL_IN_OVL0   1           /* sel_in_map[5]: второй источник */

#define WDMA0_GATE          11          /* затвор тактов disp_wdma0 */

/*
 * Формат.
 *
 * Наш пиксель — 0xAARRGGBB в слове, то есть в памяти байтами B, G, R, A.
 * По enum UNIFIED_COLOR_FMT из ddp_info.h это UFMT_BGRA8888: код формата
 * 2, своп 0. Ровно то, что мы сами пишем в L_CON оверлея, и это лишняя
 * проверка, что формат понят правильно.
 *
 * CFG: OUT_FORMAT в битах [7:4], своп бит 16, CT_EN бит 11 (для RGB не
 * нужен), EXT_MTX_EN бит 13.
 */
#define WDMA_CFG_BGRA8888   (2U << 4)

#define SNAP_W  256
#define SNAP_H  256
#define SNAP_X  100         /* внутри рабочей области, там есть что снимать */
#define SNAP_Y  300

static u32 *snap_buf;

static void snap_report(const char *when)
{
    kprintf("MDP      : WDMA0 %s: EN %08x CFG %08x SRC %08x CLIP %08x\n",
            when,
            mmio_read32(WDMA0_BASE + WDMA_EN),
            mmio_read32(WDMA0_BASE + WDMA_CFG),
            mmio_read32(WDMA0_BASE + WDMA_SRC_SIZE),
            mmio_read32(WDMA0_BASE + WDMA_CLIP_SIZE));
    kprintf("MDP      :       АДРЕС %08x ШАГ %08x СОСТ %08x ПОТОК %08x\n",
            mmio_read32(WDMA0_BASE + WDMA_DST_ADDR0),
            mmio_read32(WDMA0_BASE + WDMA_DST_W_IN_BYTE),
            mmio_read32(WDMA0_BASE + WDMA_INTSTA),
            mmio_read32(WDMA0_BASE + WDMA_FLOW_CTRL_DBG));
    usb_flush();
}

static void snapshot(void)
{
    u32 cg, mout_was, sel_was;
    u32 nonzero = 0;
    u32 i;

    if (!snap_buf) {
        snap_buf = (u32 *)pmm_alloc_dma(SNAP_W * SNAP_H * 4);
        if (!snap_buf) {
            kprintf("MDP      : СНИМОК: НЕ ХВАТИЛО ПАМЯТИ\n");
            return;
        }
    }

    /* Заполняем узнаваемым мусором: если после снимка он останется,
     * значит железо в буфер не писало вовсе, и это видно сразу. */
    for (i = 0; i < SNAP_W * SNAP_H; i++)
        snap_buf[i] = 0xDEADBEEF;

    kprintf("MDP      : СНИМОК ЭКРАНА %dx%d ИЗ (%d,%d) В 0x%08lx\n",
            SNAP_W, SNAP_H, SNAP_X, SNAP_Y, (u64)snap_buf);

    /* 1. Такты. Без них обращение к блоку повесит шину. */
    cg = gates();
    if (cg & (1U << WDMA0_GATE)) {
        mmio_write32(MMSYS_BASE + MMSYS_CG_CLR0, 1U << WDMA0_GATE);
        cg = gates();
        kprintf("MDP      : РАЗБУДИЛ disp_wdma0, ЗАТВОРЫ %08x\n", cg);
    }
    if (cg & (1U << WDMA0_GATE)) {
        kprintf("MDP      : disp_wdma0 НЕ ПРОСНУЛСЯ, ДАЛЬШЕ НЕ ИДУ\n");
        usb_flush();
        return;
    }
    usb_flush();

    /* 2. Сброс: приводим блок в известное состояние */
    mmio_write32(WDMA0_BASE + WDMA_RST, 1);
    (void)mmio_read32(WDMA0_BASE + WDMA_RST);
    mmio_write32(WDMA0_BASE + WDMA_RST, 0);

    /* 3. Настройка. Порядок и поля — как в wdma_config() вендора. */
    mmio_write32(WDMA0_BASE + WDMA_SRC_SIZE,  (2340U << 16) | 1080U);
    mmio_write32(WDMA0_BASE + WDMA_CLIP_COORD, (SNAP_Y << 16) | SNAP_X);
    mmio_write32(WDMA0_BASE + WDMA_CLIP_SIZE,  (SNAP_H << 16) | SNAP_W);
    mmio_write32(WDMA0_BASE + WDMA_CFG,        WDMA_CFG_BGRA8888);
    mmio_write32(WDMA0_BASE + WDMA_DST_ADDR0,  (u32)(u64)snap_buf);
    mmio_write32(WDMA0_BASE + WDMA_DST_W_IN_BYTE, SNAP_W * 4);
    mmio_write32(WDMA0_BASE + WDMA_ALPHA,      (1U << 31) | 0xFF);
    mmio_write32(WDMA0_BASE + WDMA_INTSTA,     0);   /* сбросить признаки */

    /* 4. Включаем приёмник ДО того, как ему что-то пошлют */
    mmio_write32(WDMA0_BASE + WDMA_EN, 1);
    snap_report("НАСТРОЕН");

    /*
     * 5. Подключаем на три кадра.
     *
     * Маска, а не выбор: прежний получатель остаётся, экран продолжает
     * показывать. Возврат безусловный — что бы ни вышло, через пятьдесят
     * миллисекунд всё как было.
     */
    mout_was = mmio_read32(MMSYS_BASE + DISP_OVL0_MOUT_EN);
    sel_was  = mmio_read32(MMSYS_BASE + DISP_WDMA0_SEL_IN);

    mmio_write32(MMSYS_BASE + DISP_WDMA0_SEL_IN, WDMA0_SEL_IN_OVL0);
    mmio_write32(MMSYS_BASE + DISP_OVL0_MOUT_EN, mout_was | OVL0_MOUT_TO_WDMA0);
    kprintf("MDP      : ПОДКЛЮЧИЛ: MOUT %08x -> %08x, SEL_IN %08x -> %d\n",
            mout_was, mout_was | OVL0_MOUT_TO_WDMA0, sel_was, WDMA0_SEL_IN_OVL0);
    usb_flush();

    task_sleep_ms(50);

    mmio_write32(MMSYS_BASE + DISP_OVL0_MOUT_EN, mout_was);
    mmio_write32(MMSYS_BASE + DISP_WDMA0_SEL_IN, sel_was);
    snap_report("ПОСЛЕ   ");
    mmio_write32(WDMA0_BASE + WDMA_EN, 0);

    kprintf("MDP      : ОТКЛЮЧИЛ, MOUT ВЕРНУЛСЯ В %08x\n",
            mmio_read32(MMSYS_BASE + DISP_OVL0_MOUT_EN));

    /* 6. Что в буфере */
    for (i = 0; i < SNAP_W * SNAP_H; i++)
        if (snap_buf[i] != 0xDEADBEEF)
            nonzero++;

    kprintf("MDP      : СЛОВ ИЗМЕНИЛОСЬ %u ИЗ %u\n",
            nonzero, SNAP_W * SNAP_H);
    kprintf("MDP      : ПЕРВЫЕ СЛОВА: %08x %08x %08x %08x\n",
            snap_buf[0], snap_buf[1], snap_buf[2], snap_buf[3]);
    kprintf("MDP      : СЕРЕДИНА:     %08x %08x %08x %08x\n",
            snap_buf[SNAP_W * SNAP_H / 2 + 0], snap_buf[SNAP_W * SNAP_H / 2 + 1],
            snap_buf[SNAP_W * SNAP_H / 2 + 2], snap_buf[SNAP_W * SNAP_H / 2 + 3]);

    if (nonzero)
        kprintf("MDP      : СНИМОК ПОЛУЧИЛСЯ\n");
    else
        kprintf("MDP      : БУФЕР НЕ ТРОНУТ — КАДР ДО WDMA0 НЕ ДОШЁЛ\n");
    usb_flush();
}

/*
 * Отчёт печатается трижды, а не один раз.
 *
 * Один раз мы уже пробовали, и он не дошёл: разведка идёт на десятой
 * секунде, а кольцо консоли шестнадцать килобайт, и пока терминал
 * подключали, отчёт вытеснило загрузочным выводом. Тот же случай, что
 * когда-то с отчётом о шине.
 *
 * Поэтому читающая часть повторяется в первых трёх пульсах, то есть
 * первые полминуты: подключился в любой момент — увидел. Действия же
 * (разбудить такты) остаются разовыми.
 *
 * Отсюда же убрана вся отработавшая диагностика: проба записи, сброс и
 * замер ширины мультиплексоров. Они свои ответы дали и записаны в
 * docs/09, а в логе занимали место, которого не хватало живому.
 */
void mdp_probe(void)
{
    static int woken;
    static int beats;
    u32 cg, want;

    /*
     * Первые полминуты молчим.
     *
     * Отчёт печатался в первых же пульсах и тонул в загрузочном выводе.
     * Кольцо консоли теперь много больше, и одного этого хватило бы, но
     * подождать заодно ничего не стоит: к шестидесятой секунде загрузка
     * давно отговорила, и отчёт выходит в тишину.
     */
    beats++;
    if (beats < 6 || beats > 8)
        return;

    if (beats == 7) {           /* снимок один раз, в середине окна */
        snapshot();
        return;
    }

    disp_routes();

    cg = gates();
    kprintf("MDP      : ЗАТВОРЫ ТАКТОВ %08x (1 = СПИТ)\n", cg);

    /*
     * Сверка перед любым действием: оверлей показывает кадр прямо
     * сейчас, значит его затвор обязан читаться нулём. Не читается —
     * таблица понята неправильно, и будить по ней ничего нельзя.
     */
    if (cg & (1U << 7)) {
        kprintf("MDP      : СВЕРКА НЕ ПРОШЛА: disp_ovl0 ЧИСЛИТСЯ СПЯЩИМ,\n");
        kprintf("MDP      : А ОН ПОКАЗЫВАЕТ КАДР. ТАБЛИЦА НЕВЕРНА, СТОЮ.\n");
        usb_flush();
        return;
    }

    want = 0;
    for (unsigned i = 0; i < CHAIN_N; i++)
        want |= 1U << chain[i].gate;

    if (!woken && (cg & want)) {
        woken = 1;
        mmio_write32(MMSYS_BASE + MMSYS_CG_CLR0, want);
        cg = gates();
        kprintf("MDP      : РАЗБУДИЛ ЦЕПОЧКУ, ЗАТВОРЫ СТАЛИ %08x\n", cg);
    }
    usb_flush();
}

#else

void mdp_probe(void) { }

#endif

