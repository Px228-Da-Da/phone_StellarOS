/*
 * main.c — точка входа ядра на Си.
 *
 * Этап 1: доказать, что наш код реально исполняется на железе:
 * поднять вывод, показать состояние процессора и не упасть.
 */
#include "types.h"
#include "io.h"
#include "uart.h"
#include "print.h"
#include "fb.h"
#include "mmu.h"
#include "gic.h"
#include "timer.h"
#include "pmm.h"
#include "kmalloc.h"
#include "string.h"
#include "smp.h"
#include "sched.h"
#include "spinlock.h"
#include "fdt.h"
#include "i2c.h"
#include "gpio.h"
#include "pmic.h"
#include "usb.h"
#include "spi.h"
#include "nvt.h"
#include "ovl.h"
#include "input.h"
#include "ui.h"
#include "charger.h"
#include "uspace.h"
#include "syscall.h"
#include "emmc.h"

#if defined(BOARD_MERLIN)
#include "soc/mt6768.h"
#define RAM_BASE    MT_RAM_BASE
#define RAM_SIZE    MT_RAM_USABLE
#else
#include "soc/qemu_virt.h"
#define RAM_BASE    QEMU_RAM_BASE
#define RAM_SIZE    QEMU_RAM_SIZE
#endif

#define OS_NAME     "VELO-OS"
#define OS_VERSION  "0.4"

/* Частота системного тика. 100 Гц — компромисс: достаточно часто, чтобы
 * планировщик на этапе 4 переключал задачи незаметно для глаза, и достаточно
 * редко, чтобы обработчик прерывания не съедал время сам на себя. */
#define TIMER_HZ    100

/*
 * Сторожевой таймер MediaTek.
 *
 * Его запускает preloader, и дальше его обязан гладить тот, кто получил
 * управление. Linux делает это драйвером mtk_wdt, а мы не делаем ничего —
 * поэтому железо перезагружает телефон через несколько секунд само, как бы
 * хорошо ни работал наш код. Выключаем его раньше всего остального.
 */
static void watchdog_disable(void)
{
#if defined(BOARD_MERLIN)
    u32 mode = mmio_read32(MT_TOPRGU_BASE + WDT_MODE);

    mode &= ~(WDT_MODE_EN | WDT_MODE_IRQ | WDT_MODE_DUAL);
    mode |= WDT_MODE_KEY;                   /* без ключа запись игнорируется */
    mmio_write32(MT_TOPRGU_BASE + WDT_MODE, mode);
    dsb();
#endif
}

/*
 * Отладка на живом железе без единого канала вывода.
 *
 * До UART на этом телефоне надо паяться, экран может быть ровно тем, что
 * не работает. Остаётся один наблюдаемый бит: перезагрузился телефон или
 * завис. Сборка с HALT_AT=N останавливает ядро на этапе N, и тогда:
 *   телефон висит, не перезагружаясь  -> до этой точки мы дошли;
 *   перезагрузился                    -> упали раньше.
 * Двоичным поиском по номеру этапа находим сломанную подсистему.
 * Работает только при выключенном стороже — иначе ресет придёт всегда.
 */
static void delay_ms(u32 ms);           /* определена ниже */

#ifdef HALT_AT
#define HALT_AT_OR_ZERO (HALT_AT)
#else
#define HALT_AT_OR_ZERO 0
#endif

#ifndef HALT_DELAY_MS
#define HALT_DELAY_MS 8000              /* сколько держать цвет перед сбросом */
#endif

#ifdef HALT_AT
/*
 * Экран — единственный канал связи с телефоном: UART требует пайки, а по USB
 * ядро молчит. Поэтому точка залипания не просто виснет, а заливает экран
 * цветом, кодирующим номер этапа. Увиденный цвет отвечает сразу на два
 * вопроса: докуда дошло исполнение и верно ли найден фреймбуфер.
 *
 * Чёрный экран на этапе 4 и позже означает, что fb_init не нашёл буфер —
 * это тоже ответ, просто отрицательный.
 */
static void halt_forever(unsigned stage)
{
    static const u32 stage_color[] = {
        COLOR_BLACK,        /* 0 — не используется              */
        COLOR_RED,          /* 1 — сторож выключен              */
        COLOR_YELLOW,       /* 2 — UART поднят                  */
        COLOR_GREEN,        /* 3 — MMU и кэши включены          */
        COLOR_CYAN,         /* 4 — фреймбуфер инициализирован   */
        COLOR_BLUE,         /* 5 — аллокатор памяти поднят      */
        COLOR_WHITE,        /* 6 — GIC и таймер                 */
        0x00FF00FF,         /* 7 — восемь ядер разбужены        */
    };

    /* Заливаем только те этапы, для которых заведён цвет. Для остальных
     * экран не трогаем: там уже напечатана диагностика, и стереть её
     * заливкой значит потерять единственное, ради чего мы остановились. */
    if (fb_available() && stage < ARRAY_SIZE(stage_color))
        fb_clear(stage_color[stage]);

    irq_disable();

    /* Даже замерев, продолжаем отвечать хосту: иначе COM-порт нельзя
     * будет открыть и прочитать то, ради чего мы остановились. */
    usb_flush();

#ifdef RESET_AT_HALT
    /*
     * Сигнал, не зависящий от экрана.
     *
     * Чёрный экран одинаково выглядит и когда мы сюда дошли, но фреймбуфер
     * не найден, и когда мы не дошли вовсе. Различить их можно только
     * событием, видимым снаружи, — и такое у нас есть: мы умеем управлять
     * сторожевым таймером, а значит можем осознанно вызвать сброс.
     *
     *   телефон уходит в цикл перезагрузок -> до этапа дошли;
     *   телефон тихо висит                 -> упали раньше.
     *
     * Пауза перед сбросом должна быть заведомо заметной: секунды не хватает,
     * в мельтешении перезагрузок вспышку цвета просто не поймать глазом.
     */
    delay_ms(HALT_DELAY_MS);
    mmio_write32(MT_TOPRGU_BASE + WDT_SWRST, WDT_SWRST_KEY);
    dsb();
#endif

    for (;;)
        usb_poll();
}
#define HALT_STAGE(n)   do { if ((n) == (HALT_AT)) halt_forever(n); } while (0)
#else
#define HALT_STAGE(n)   do { } while (0)
#endif

static void heartbeat_task(void *arg);
static void heartbeat_polling(void);
static int  irq_works(void);
static void mem_selftest(void);
static void worker_task(void *arg);
static void respawn_task(void *arg);
static void boot_task(void *arg);
static void memory_setup(u64 dtb_phys);
static void fb_flip_demo(void);
static void fb_flip_probe(void);
static void fb_stride_test(void);
static void fb_banding_test(void);
static void i2c0_dump(void);
static void i2c0_scan(void);
static void gpio_check(void);
static void touch_wake(void);
static void pmic_probe(void);
static void touch_power_on(void);
static void touch_power_on_only(void);

/* Состояние демонстрационной задачи: у каждой своё, общего — только счётчики */
struct worker {
    u32 index;
    u32 cpu;                    /* на каком ядре отработала последний круг */
    u64 rounds;
    u64 count;                  /* личный счётчик: с ним сверяем общий */
};

/* Задач намеренно больше, чем ядер даже у Helio G85: пока задач меньше,
 * каждая просто сидит на своём ядре, и ни вытеснения, ни переездов
 * между ядрами в дампе не увидеть — планировщику нечего решать. */
static struct worker workers[10];
static const char *worker_names[10] = {
    "счёт-01", "счёт-02", "счёт-03", "счёт-04", "счёт-05",
    "счёт-06", "счёт-07", "счёт-08", "счёт-09", "счёт-10",
};

/* Заголовок DTB: big-endian, магия 0xd00dfeed */
struct fdt_header {
    u32 magic;
    u32 totalsize;
    u32 off_dt_struct;
    u32 off_dt_strings;
    u32 off_mem_rsvmap;
    u32 version;
};

static u32 be32(u32 v)
{
    return ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) |
           ((v & 0x00FF0000u) >> 8)  | ((v & 0xFF000000u) >> 24);
}

/* Активное ожидание по системному счётчику: он тикает и без настроенного таймера */
static void delay_ms(u32 ms)
{
    u64 freq = read_cntfrq();
    u64 target = read_cntpct() + (freq / 1000) * ms;

    while (read_cntpct() < target)
        usb_poll();     /* иначе за время паузы хост объявит порт мёртвым */
}


/*
 * Замер скорости записи в память. Гоняем один и тот же цикл до и после
 * включения кэшей — разница показывает, что MMU реально заработал.
 */
static u32 bench_buf[16 * 1024];            /* 64 КБ */

static u64 bench_memfill(void)
{
    u64 t0 = read_cntpct();

    for (u32 pass = 0; pass < 64; pass++)
        for (u32 i = 0; i < ARRAY_SIZE(bench_buf); i++)
            bench_buf[i] = i + pass;

    return read_cntpct() - t0;
}

/* Перевод тиков счётчика в микросекунды */
static u64 ticks_to_us(u64 ticks)
{
    return (ticks * 1000000UL) / read_cntfrq();
}

static void banner(void)
{
    kprintf("\n");
    kprintf("================================\n");
    kprintf("  %s v%s\n", OS_NAME, OS_VERSION);
    kprintf("  AARCH64 BARE METAL KERNEL\n");
    kprintf("================================\n\n");
}

static void dump_cpu(void)
{
    u64 mpidr = read_mpidr();

    kprintf("EL       : %lu\n", read_currentel());
    kprintf("MPIDR    : %016lx (CORE %lu)\n", mpidr, mpidr & 0xFF);
    kprintf("CNTFRQ   : %lu HZ\n", read_cntfrq());
    kprintf("CNTPCT   : %lu\n", read_cntpct());
}

static void dump_dtb(u64 dtb_phys)
{
    struct fdt_header *h = (struct fdt_header *)(uintptr_t)dtb_phys;

    kprintf("DTB      : %p\n", (void *)(uintptr_t)dtb_phys);
    if (!dtb_phys) {
        kprintf("DTB      : NOT PASSED\n");
        return;
    }
    if (be32(h->magic) != 0xD00DFEEDu) {
        kprintf("DTB      : BAD MAGIC %08x\n", be32(h->magic));
        return;
    }
    kprintf("DTB SIZE : %u BYTES, VER %u\n", be32(h->totalsize), be32(h->version));
}

static void dump_fb(void)
{
    u64 base;
    u32 w, h, stride;

    if (!fb_available()) {
        kprintf("SCREEN   : NONE (UART ONLY)\n");
        return;
    }
    fb_info(&base, &w, &h, &stride);
    kprintf("SCREEN   : %ux%u STRIDE %u\n", w, h, stride);
    kprintf("FB ADDR  : %p\n", (void *)(uintptr_t)base);
}

/*
 * Разметка памяти по device tree.
 *
 * Раньше объём и база памяти были константами в карте SoC. Теперь их
 * сообщает само устройство, и главное — вместе с ними приходит список
 * областей, которые трогать нельзя: ATF, доверенная память, буферы модема,
 * фреймбуфер. На merlin таких областей два десятка, они разбросаны внутри
 * той же DRAM и закрыты контроллером EMI. Выдать такую область под кучу —
 * это не порча данных, а мгновенная перезагрузка без единого сообщения.
 */
static void memory_setup(u64 dtb_phys)
{
    struct fdt_region mem[4];
    struct fdt_region res[48];
    u32 nmem = 0, nres = 0;
    u64 base = RAM_BASE, size = RAM_SIZE;

    if (fdt_check(dtb_phys) == 0) {
        const char *model = fdt_root_string(dtb_phys, "model");

        if (model)
            kprintf("ПЛАТА    : %s\n", model);

        nmem = fdt_memory(dtb_phys, mem, ARRAY_SIZE(mem));
        nres = fdt_reserved(dtb_phys, res, ARRAY_SIZE(res));
    }

    if (nmem) {
        base = mem[0].base;
        size = mem[0].size;
        kprintf("ПАМЯТЬ DTB: %p .. %p (%lu МБ), ОБЛАСТЕЙ %u\n",
                (void *)(uintptr_t)base, (void *)(uintptr_t)(base + size),
                size / (1024 * 1024), nmem);

        /* Несколько несмежных банков pmm пока не умеет: он ведёт одну
         * карту на непрерывный диапазон. Молчать об этом нельзя —
         * пропавшая память должна быть видна, а не потеряна тихо. */
        for (u32 i = 1; i < nmem; i++)
            kprintf("           ЕЩЁ %p (%lu МБ) — НЕ ИСПОЛЬЗУЕТСЯ\n",
                    (void *)(uintptr_t)mem[i].base, mem[i].size / (1024 * 1024));
    } else {
        kprintf("ПАМЯТЬ    : В DTB НЕ НАЙДЕНА, БЕРУ ВСТРОЕННУЮ КАРТУ\n");
    }

    /*
     * Дерево описывает физическую память, а таблицы трансляции строятся
     * при загрузке по константе из карты SoC. Если устройство окажется
     * богаче, чем мы отобразили, аллокатор начнёт раздавать адреса, для
     * которых трансляции нет: первое же обращение — ошибка доступа далеко
     * от места настоящей причины. Поэтому обрезаем и говорим об этом вслух.
     */
    if (base + size > mmu_ram_limit()) {
        u64 fit = base < mmu_ram_limit() ? mmu_ram_limit() - base : 0;

        kprintf("ПАМЯТЬ    : ОТОБРАЖЕНО ДО %p, ОБРЕЗАЮ %lu МБ ДО %lu МБ\n",
                (void *)(uintptr_t)mmu_ram_limit(),
                size / (1024 * 1024), fit / (1024 * 1024));
        size = fit;
    }

    if (!size) {
        kprintf("ПАМЯТЬ    : РАЗДАВАТЬ НЕЧЕГО\n");
        return;
    }

#if defined(BOARD_MERLIN)
    /*
     * На телефоне без списка защищённых областей аллокатор запускать нельзя.
     * Никакой «осторожный объём» тут не спасает: ATF и доверенная память
     * лежат в первых же сотнях мегабайт, вперемешку с обычной DRAM.
     * Лучше остаться без кучи и задач, чем получить ресет на ровном месте.
     */
    if (!nres) {
        kprintf("ПАМЯТЬ    : СПИСОК ЗАЩИЩЁННЫХ ОБЛАСТЕЙ ПУСТ — PMM НЕ ЗАПУСКАЮ\n");
        kprintf("            СМ. docs/01-safety.md, ПРАВИЛО 4\n");
        return;
    }
#endif

    if (pmm_init(base, size, dtb_phys) != 0)
        return;

    for (u32 i = 0; i < nres; i++)
        pmm_reserve(res[i].base, res[i].size);

    if (nres)
        kprintf("ЗАЩИЩЕНО : %u ОБЛАСТЕЙ ИЗ DTB\n", nres);

    /* Фреймбуфер загрузчика в /reserved-memory обычно есть, но полагаться
     * на это не станем: свой адрес мы и так знаем от контроллера дисплея. */
    if (fb_available()) {
        u64 fb_base;
        u32 w, h, stride;

        fb_info(&fb_base, &w, &h, &stride);
        pmm_reserve(fb_base, (u64)stride * h * 4);
    }

    mem_selftest();
}

/* Цветные полосы: видны, даже если шрифт вдруг рисуется неверно */
static void test_pattern(void)
{
    static const u32 colors[] = {
        COLOR_RED, COLOR_GREEN, COLOR_BLUE, COLOR_YELLOW, COLOR_CYAN, COLOR_WHITE
    };
    u64 base; u32 w, h, stride;

    if (!fb_available())
        return;

    fb_info(&base, &w, &h, &stride);
    for (u32 i = 0; i < ARRAY_SIZE(colors); i++)
        fb_fill_rect(i * (w / ARRAY_SIZE(colors)), h - 120,
                     w / ARRAY_SIZE(colors), 120, colors[i]);
}

/*
 * Работает ли канал к PMIC.
 *
 * Смещения регистров обёртки PWRAP у разных поколений MediaTek разные, и
 * какое из них у MT6768, по дереву не понять. Поэтому проверяем оба
 * вероятных варианта за одну прошивку: просим у PMIC его собственный
 * идентификатор и смотрим, какой набор смещений даст осмысленный ответ.
 *
 * Идентификатор выбран намеренно: это чтение, оно ничего не меняет, а
 * значение заведомо не ноль и не 0xFFFF — спутать с мусором нельзя.
 */
/*
 * Только питание и сброс, без ожидания.
 *
 * Отдельно от touch_power_on: та тридцать секунд следит за линией
 * прерывания, а перед разговором по SPI ждать нечего — контроллеру
 * нужно лишь напряжение и снятый сброс.
 */
static void touch_power_on_only(void)
{
#if defined(BOARD_MERLIN)
    u16 con0 = 0;

    if (pmic_read(MT6358_LDO_VLDO28_CON0, &con0) != 0)
        return;
    if (!(con0 & 1)) {
        pmic_write(MT6358_LDO_VLDO28_CON0, (u16)(con0 | 1));
        delay_ms(5);
        pmic_read(MT6358_LDO_VLDO28_CON0, &con0);
    }
    kprintf("ТАЧ: ПИТАНИЕ %s\n", (con0 & 1) ? "ЕСТЬ" : "НЕТ");

    /* Сброс и линию прерывания дальше настраивает сам драйвер:
     * у него выдержки взяты из вендорного кода, а не подобраны
     * на глаз. */
#endif
}

/*
 * Проверка тачскрина: рисуем там, где палец.
 *
 * Опрашиваем без оглядки на линию прерывания. Она активна низким уровнем
 * и по-хорошему должна отсекать пустые опросы, но полагаться на неё, пока
 * не увидели ни одного касания, рано: если она заведена не так, как мы
 * думаем, мы просто не пойдём на шину и решим, что тачскрин молчит.
 * Сначала убедимся, что данные идут, и только потом сэкономим на опросе.
 */
static void touch_demo(u32 seconds)
{
#if defined(BOARD_MERLIN)
    struct nvt_touch t[10];
    u64 end = read_cntpct() + read_cntfrq() * seconds;
    u64 next_report = 0, next_poll = 0;
    u32 events = 0, polls = 0, irq_low = 0;

    fb_clear(COLOR_BLACK);
    kprintf("ТАЧ: КАСАЙСЯ ЭКРАНА %u СЕК\n", seconds);

    while (read_cntpct() < end) {
        u64 now = read_cntpct();
        int n;

        usb_poll();

        /* Опрашиваем сто раз в секунду. Без выдержки цикл успевал под
         * сто тысяч опросов в секунду — столько микросхема не отдаёт, и
         * между редкими удачными чтениями шла сплошная единица. */
        if (now < next_poll)
            continue;
        next_poll = now + read_cntfrq() / 100;
        polls++;
        if (!gpio_read(NVT_GPIO_IRQ))
            irq_low++;

        n = nvt_get_touches(t, 10);

        if (n > 0) {
            events++;
            for (int i = 0; i < n; i++) {
                u32 half = 12;
                u32 x = t[i].x > half ? t[i].x - half : 0;
                u32 y = t[i].y > half ? t[i].y - half : 0;
                /* Цвет по номеру пальца: сразу видно, что их различают */
                static const u32 palette[5] = {
                    0xFF00FF00, 0xFFFF4040, 0xFF4080FF, 0xFFFFFF00, 0xFFFF00FF
                };

                fb_fill_rect(x, y, half * 2, half * 2, palette[t[i].id % 5]);
            }
        }

        /* Раз в секунду отчитываемся, даже если касаний нет: молчание
         * ничего не говорит, а состояние линии и сырые байты — говорят. */
        if (read_cntpct() >= next_report) {
            u8 raw[8];

            next_report = read_cntpct() + read_cntfrq();
            nvt_peek_event(raw, sizeof(raw));
            kprintf("ТАЧ: ПАЛЬЦЕВ %d, ЛИНИЯ %d, ОПРОСОВ %u НИЗКИХ %u, "
                    "БАЙТЫ %02x %02x %02x %02x %02x %02x %02x %02x\n",
                    n, gpio_read(NVT_GPIO_IRQ), polls, irq_low,
                    raw[0], raw[1], raw[2], raw[3],
                    raw[4], raw[5], raw[6], raw[7]);
            if (n > 0)
                kprintf("ТАЧ: ПЕРВЫЙ #%u В %u,%u ШИРИНА %u\n",
                        t[0].id, t[0].x, t[0].y, t[0].w);
        }
    }
    kprintf("ТАЧ: СОБЫТИЙ ЗА %u СЕК: %u ИЗ %u ОПРОСОВ\n",
            seconds, events, polls);
#else
    (void)seconds;
#endif
}

/*
 * Включить питание тачскрина и разбудить его.
 *
 * Питание идёт от регулятора VLDO28 внутри PMIC — это выяснилось из
 * свойства vtouch-supply в дереве устройства. Загрузчик его не поднимает:
 * панель светится от другой линии, а тач ему не нужен.
 *
 * Это первая наша ЗАПИСЬ в PMIC. Делаем её узко: читаем регистр, ставим
 * один бит включения, пишем обратно. Никаких других полей не трогаем —
 * в PMIC соседние биты управляют питанием памяти и самого процессора.
 */
static void touch_power_on(void)
{
#if defined(BOARD_MERLIN)
    u16 con0 = 0;

    if (pmic_read(MT6358_LDO_VLDO28_CON0, &con0) != 0) {
        kprintf("PMIC: ЧТЕНИЕ НЕ ВЫШЛО\n");
        return;
    }
    kprintf("VLDO28: БЫЛО %x (%s)\n", con0,
            (con0 & 1) ? "ВКЛ" : "ВЫКЛ");

    if (!(con0 & 1)) {
        pmic_write(MT6358_LDO_VLDO28_CON0, (u16)(con0 | 1));
        delay_ms(5);                    /* регулятору нужно время выйти */
        pmic_read(MT6358_LDO_VLDO28_CON0, &con0);
        kprintf("VLDO28: СТАЛО %x (%s)\n", con0,
                (con0 & 1) ? "ВКЛ" : "ВЫКЛ");
    }

    if (!(con0 & 1)) {
        kprintf("VLDO28: ВКЛЮЧИТЬ НЕ УДАЛОСЬ\n");
        return;
    }

    /* Питание есть — теперь снимаем сброс и смотрим на линию прерывания */
    kprintf("ТАЧ: СНИМАЮ СБРОС\n");
    gpio_set_dir(NVT_GPIO_RESET, GPIO_OUT);
    gpio_write(NVT_GPIO_RESET, 0);
    delay_ms(20);
    gpio_write(NVT_GPIO_RESET, 1);
    delay_ms(200);

    gpio_set_dir(NVT_GPIO_IRQ, GPIO_IN);

    {
        int prev = gpio_read(NVT_GPIO_IRQ);
        u32 changes = 0;
        u64 end = read_cntpct() + read_cntfrq() * 30;

        kprintf("ТАЧ: ЛИНИЯ %d, КАСАЙСЯ ЭКРАНА 30 СЕК\n", prev);
        while (read_cntpct() < end) {
            int now = gpio_read(NVT_GPIO_IRQ);

            if (now != prev) {
                changes++;
                prev = now;
                if (changes < 40)
                    kprintf("ТАЧ: ЛИНИЯ -> %d (%u)\n", now, changes);
            }
        }
        kprintf("ТАЧ: ИТОГО ПЕРЕКЛЮЧЕНИЙ %u\n", changes);
    }
#endif
}

static void pmic_probe(void)
{
#if defined(BOARD_MERLIN)
    u16 id = 0, con0 = 0, en = 0;

    /*
     * Канал связи с PMIC найден дампом всего блока: загрузчик пользуется
     * третьим каналом (0xC20), и там остался след его последней команды.
     * Готовность отмечается старшим битом ответа — это и было причиной
     * того, что перебор смещений ничего не находил.
     *
     * Проверяем канал самым безобидным способом: спрашиваем у PMIC его
     * собственный идентификатор. Это чтение, оно ничего не меняет.
     */
    if (pmic_read(MT6358_SWCID, &id) != 0) {
        kprintf("PMIC: ОТВЕТА НЕТ\n");
        return;
    }
    /* Ждём 0x5820: это настоящий идентификатор MT6358. Если придёт он —
     * значит задержка вылечила отставание ответов на один запрос. */
    kprintf("PMIC: ID %x %s\n", id, id == 0x5820 ? "(MT6358, ВЕРНО)" : "(?)");

    if (pmic_read(MT6358_LDO_VLDO28_CON0, &con0) == 0)
        kprintf("VLDO28 CON0 %x -> ПИТАНИЕ %s\n", con0,
                (con0 & 1) ? "ВКЛЮЧЕНО" : "ВЫКЛЮЧЕНО");
    if (pmic_read(MT6358_LDO_VLDO28_OP_EN, &en) == 0)
        kprintf("VLDO28 OP_EN %x\n", en);
#endif
}

/*
 * Оживает ли тачскрин после снятия сброса.
 *
 * Проверка GPIO показала, что загрузчик держит вывод 92 в нуле, то есть
 * контроллер тача выключен. У таких микросхем есть собственная прошивка,
 * и после снятия сброса они обычно начинают сами дёргать линию прерывания
 * при касании — ещё до того, как мы скажем им хоть слово по SPI.
 *
 * Если это так, то мы получим ввод, не написав ни строчки драйвера SPI.
 * Экран красим по состоянию линии: смотреть на цвет проще, чем на числа,
 * и видно мгновенно.
 */
static void touch_wake(void)
{
#if defined(BOARD_MERLIN)
    u32 changes = 0;
    int prev;
    u64 deadline;

    kprintf("ТАЧ: СНИМАЮ СБРОС (ВЫВОД %u)\n", NVT_GPIO_RESET);
    gpio_set_dir(NVT_GPIO_RESET, GPIO_OUT);
    gpio_write(NVT_GPIO_RESET, 0);      /* убедимся, что действительно в сбросе */
    delay_ms(20);
    gpio_write(NVT_GPIO_RESET, 1);      /* отпускаем */
    delay_ms(200);                      /* даём прошивке контроллера подняться */

    gpio_set_dir(NVT_GPIO_IRQ, GPIO_IN);
    prev = gpio_read(NVT_GPIO_IRQ);
    kprintf("ТАЧ: ЛИНИЯ ПРЕРЫВАНИЯ %d, КАСАЙСЯ ЭКРАНА\n", prev);

    /* Полминуты следим за линией и красим экран по её состоянию */
    deadline = read_cntpct() + read_cntfrq() * 30;
    while (read_cntpct() < deadline) {
        int now = gpio_read(NVT_GPIO_IRQ);

        if (now != prev) {
            changes++;
            prev = now;
            /* Линия у таких контроллеров активна нулём: 0 значит «есть что
             * забрать», то есть палец на экране. */
            fb_fill_rect(0, 0, 1080, 300, now ? COLOR_BLUE : COLOR_GREEN);
        }
    }

    kprintf("ТАЧ: ЛИНИЯ МЕНЯЛАСЬ %u РАЗ\n", changes);
#endif
}

/*
 * Верна ли раскладка регистров GPIO.
 *
 * Проверяем не абстрактно, а по заведомо известным выводам: у тачскрина
 * линия прерывания (вывод 1) обязана быть входом, а линия сброса
 * (вывод 92) — выходом. Номера взяты из раздела dtbo официальной прошивки,
 * сомнений в них нет. Значит если направления совпали, смещения угаданы
 * верно, а если нет — раскладка неправильная, и писать в неё нельзя.
 *
 * Только чтение: ни одной записи, пока не убедимся.
 */
static void gpio_check(void)
{
#if defined(BOARD_MERLIN)
    static const struct { u32 pin; const char *what; int want_dir; } pins[] = {
        { NVT_GPIO_IRQ,   "ТАЧ ПРЕРЫВ", GPIO_IN  },
        { NVT_GPIO_RESET, "ТАЧ СБРОС ", GPIO_OUT },
    };

    kprintf("GPIO БАЗА 0x%08lx\n", (u64)MT_GPIO_BASE);
    for (u32 i = 0; i < ARRAY_SIZE(pins); i++) {
        int dir  = gpio_get_dir(pins[i].pin);
        int mode = gpio_get_mode(pins[i].pin);
        int in   = gpio_read(pins[i].pin);
        int out  = gpio_read_out(pins[i].pin);

        kprintf("%s %3u: НАПР %s РЕЖИМ %d ВХОД %d ВЫХОД %d %s\n",
                pins[i].what, pins[i].pin,
                dir == GPIO_OUT ? "ВЫХ" : "ВХ ", mode, in, out,
                dir == pins[i].want_dir ? "СОВПАЛО" : "НЕ СОВПАЛО");
    }
#endif
}

/*
 * Кто сидит на шине I2C0.
 *
 * Опрашиваем каждый адрес и смотрим, подтвердит ли его кто-нибудь. Это
 * тот же приём, что у i2cdetect в Linux, и он отвечает на вопрос, который
 * из дерева устройства однозначно не следует: там описаны и Goodix на
 * I2C по адресу 0x5D, и Novatek на SPI, а распаян один.
 *
 * Диапазон 0x08..0x77 — рабочая часть адресного пространства I2C;
 * края зарезервированы стандартом, трогать их незачем.
 *
 * Печатаем короткий список, а не дамп: числа с экрана телефона читаются
 * плохо, и чем меньше их, тем надёжнее.
 */
extern u32 i2c_last_stat, i2c_last_spins, i2c_stat_before, i2c_ctrl_orig;

static void i2c0_scan(void)
{
#if defined(BOARD_MERLIN)
    /* Сперва разбираемся, работает ли определение ответа вообще.
     * Пробуем два адреса: 0x5d — тот, где по дереву должен быть тачскрин,
     * и 0x11 — заведомо пустой. Если статус и число оборотов у них
     * одинаковые, значит мы читаем не тот регистр и «ответили все». */
    static const u8 test[] = { 0x5d, 0x11 };
    u32 found = 0;

    for (u32 i = 0; i < ARRAY_SIZE(test); i++) {
        int r = i2c_probe(MT_I2C0_BASE, test[i]);

        kprintf("I2C %02x: РЕЗ %d СТАТ %x УПР %x ОБОРОТОВ %u\n",
                test[i], r, i2c_last_stat, i2c_ctrl_orig, i2c_last_spins);
    }

    kprintf("I2C0 ПОИСК: ");
    for (u8 a = 0x08; a <= 0x77; a++) {
        if (i2c_probe(MT_I2C0_BASE, a) == 0) {
            kprintf("%x ", a);
            found++;
        }
    }
    if (!found)
        kprintf("НИКОГО");
    kprintf("\n");
    kprintf("I2C0 НАЙДЕНО: %u\n", found);
#endif
}

/*
 * Жив ли контроллер I2C0.
 *
 * Первый шаг к тачскрину, и намеренно безопасный: только чтение регистров,
 * ни одной записи. Если я ошибся картой регистров, ничего не сломается.
 *
 * Что говорит результат:
 *   везде нули        -> блок не тактируется, LK его не включил, и прежде
 *                        чем что-то слать, надо поднять ему clock;
 *   осмысленные числа -> контроллер жив и настроен загрузчиком, можно
 *                        переходить к обмену.
 *
 * Адрес и наличие блока DMA взяты из дерева самого устройства:
 *   i2c0@11007000 reg = <0x11007000 0x1000  0x11000080 0x80>
 */
static void i2c0_dump(void)
{
#if defined(BOARD_MERLIN)
    kprintf("I2C0 @ 0x%08lx\n", (u64)MT_I2C0_BASE);
    for (u32 off = 0; off < 0x60; off += 0x10) {
        kprintf("  +%02x: %08x %08x %08x %08x\n", off,
                mmio_read32(MT_I2C0_BASE + off + 0x0),
                mmio_read32(MT_I2C0_BASE + off + 0x4),
                mmio_read32(MT_I2C0_BASE + off + 0x8),
                mmio_read32(MT_I2C0_BASE + off + 0xC));
    }
    kprintf("DMA0 @ 0x11000080\n");
    for (u32 off = 0; off < 0x20; off += 0x10) {
        kprintf("  +%02x: %08x %08x %08x %08x\n", off,
                mmio_read32(0x11000080UL + off + 0x0),
                mmio_read32(0x11000080UL + off + 0x4),
                mmio_read32(0x11000080UL + off + 0x8),
                mmio_read32(0x11000080UL + off + 0xC));
    }
#endif
}

/*
 * Полосы от нас или от матрицы.
 *
 * Геометрия проверена: stride верный, заливки ложатся ровно. Значит полосы
 * на однородном фоне могут идти от самой панели — на тёмных цветах IPS
 * часто дают видимую неравномерность яркости.
 *
 * Делим экран на четыре поля по яркости, от почти чёрного к белому. Если
 * полосы видны только на тёмных, дело в матрице и чинить нечего. Если они
 * ровные на всех четырёх — источник в нашей записи в память.
 */
static void fb_banding_test(void)
{
    u64 base; u32 w, h, stride;
    u32 band;

    if (!fb_available())
        return;

    fb_info(&base, &w, &h, &stride);
    band = h / 4;

    fb_fill_rect(0, 0 * band, w, band, 0xFF101840);   /* тёмно-синий, наш фон */
    fb_fill_rect(0, 1 * band, w, band, 0xFF0000FF);   /* яркий синий          */
    fb_fill_rect(0, 2 * band, w, band, 0xFF808080);   /* серый                */
    fb_fill_rect(0, 3 * band, w, h - 3 * band, 0xFFFFFFFF); /* белый          */
}

/*
 * Верен ли шаг строки (stride).
 *
 * Рисуем сетку из заведомо прямых линий: две вертикальные по краям и
 * горизонтальные через каждые 200 строк. Адрес пикселя считается как
 * base + y*stride + x, поэтому ошибка в stride видна сразу:
 *
 *   линии строго прямые   -> шаг строки верный;
 *   вертикальные наискось -> stride не тот, и по наклону виден настоящий:
 *                            смещение на строку = разница шагов.
 *
 * Это надёжнее любых рассуждений о том, что показывает регистр PITCH.
 */
static void fb_stride_test(void)
{
    u64 base; u32 w, h, stride;

    if (!fb_available())
        return;

    fb_info(&base, &w, &h, &stride);
    fb_clear(COLOR_BLACK);

    /* Вертикальные по краям: на них наклон заметнее всего */
    fb_fill_rect(0, 0, 8, h, COLOR_GREEN);
    fb_fill_rect(w - 8, 0, 8, h, COLOR_GREEN);

    /* Горизонтальные через каждые 200 строк */
    for (u32 y = 0; y < h; y += 200)
        fb_fill_rect(0, y, w, 4, COLOR_RED);

    /* Квадрат в углу: если stride врёт, он расползётся в параллелограмм */
    fb_fill_rect(w / 2 - 100, h / 2 - 100, 200, 200, COLOR_CYAN);
}

/*
 * Работает ли подмена буфера вообще.
 *
 * Самый прямой опыт, какой можно поставить: заливаем НЕВИДИМЫЙ буфер
 * сплошным красным, переключаемся на него и больше ничего не трогаем.
 *
 *   экран стал целиком красным -> запись в OVL_L0_ADDR переключает показ,
 *                                 page flip настоящий;
 *   на экране остался текст     -> контроллер продолжает показывать прежний
 *                                 буфер, и подмену надо проводить через
 *                                 DISP_MUTEX, а не простой записью в регистр.
 *
 * Ответ нельзя спутать: сплошной красный и консольный текст не похожи ни
 * при какой засветке камеры.
 */
static void fb_flip_probe(void)
{
    if (!fb_available())
        return;
    if (fb_double_buffer(1) != 0) {
        kprintf("ЭКРАН    : ВТОРОГО БУФЕРА НЕТ\n");
        return;
    }
    fb_clear(COLOR_RED);
    fb_flip();
}

/*
 * Проверка двойной буферизации.
 *
 * Полоса, едущая сверху вниз: каждый кадр экран стирается и рисуется заново.
 * Без второго буфера это гарантированный разрыв — контроллер дисплея выводит
 * буфер прямо в процессе того, как мы его перерисовываем, и на экране
 * оказывается смесь двух кадров. С двойной буферизацией показывается только
 * готовое, и полоса едет чисто.
 *
 * Заодно меряем, сколько стоит сам показ кадра: на телефоне это запись
 * одного регистра, в эмуляторе — копирование всего буфера, и разница
 * в цифрах должна быть очень заметной.
 */
/*
 * Аппаратная композиция: панель едет по экрану, а процессор не рисует.
 *
 * Разбито на этапы с паузами: «слоя не видно» само по себе ничего не
 * объясняет, а вот какой именно из них не виден — объясняет многое.
 */
#if defined(BOARD_MERLIN)
static u32 *ovl_panel;                  /* содержимое подвижного слоя */
#endif

static void ovl_demo(void)
{
#if defined(BOARD_MERLIN)
    const u32 PW = 480, PH = 320;       /* размер панели */
    const u32 frames = 180;             /* три секунды на шестидесяти */
    u64 base; u32 w, h, stride;
    u64 span = 2UL * 1024 * 1024;
    u64 move_ticks = 0;
    u32 mid;

    if (!fb_available())
        return;

    fb_info(&base, &w, &h, &stride);
    if (w < PW || h < PH)
        return;

    /* Память под панель. Контроллер дисплея читает её мимо кэшей
     * процессора, поэтому область обязана быть некэшируемой — ровно
     * та же причина, что и у второго буфера кадра.
     *
     * Берём целыми блоками по два мегабайта: некэшируемой память
     * помечается блоками такого размера, и соседей у буфера быть не
     * должно — иначе данные ядра станут некэшируемыми вместе с ним. */
    ovl_panel = pmm_alloc_dma(PW * PH * 4);
    if (!ovl_panel) {
        kprintf("OVL: НЕТ ПАМЯТИ ПОД ПАНЕЛЬ\n");
        return;
    }
    mmu_set_range_nc((u64)(uintptr_t)ovl_panel, span);

    /* Рисуем панель один раз: рамка и заливка с переходом по высоте */
    for (u32 py = 0; py < PH; py++) {
        for (u32 px = 0; px < PW; px++) {
            u32 c;

            if (px < 4 || py < 4 || px >= PW - 4 || py >= PH - 4)
                c = 0xFFFFFFFF;                     /* рамка */
            else
                c = 0xFF000000 | (0x30 << 16) | ((0x40 + py / 3) << 8) | 0xC0;
            ovl_panel[py * PW + px] = c;
        }
    }
    dsb();

    /*
     * Сверка содержимого — сразу и через паузу.
     *
     * Если между двумя чтениями пиксели обнулятся, значит в кэше остались
     * грязные строки от обнуления страниц аллокатором: мы писали уже мимо
     * кэша, а вытеснение старой строки пришло позже и затёрло записанное.
     * Догадаться об этом по чёрному экрану невозможно, а два числа
     * отвечают сразу.
     */
    mid = (PH / 2) * PW + PW / 2;
    kprintf("OVL: ПАНЕЛЬ %p, УГОЛ %08x СЕРЕДИНА %08x\n",
            (void *)ovl_panel, ovl_panel[0], ovl_panel[mid]);
    delay_ms(500);
    kprintf("OVL: ЧЕРЕЗ ПОЛСЕКУНДЫ  УГОЛ %08x СЕРЕДИНА %08x\n",
            ovl_panel[0], ovl_panel[mid]);

    /* Фон под слоями — основной кадр, он у нас уже есть */
    fb_clear(0xFF101840);
    ovl_dump();

    kprintf("OVL: ЭТАП 1 — ЗАТЕМНЕНИЕ ВО ВЕСЬ ЭКРАН, 3 СЕК\n");
    ovl_layer_color(2, 0x90000000, 0, 0, w, h);
    delay_ms(3000);
    ovl_layer_off(2);

    kprintf("OVL: ЭТАП 2 — ПАНЕЛЬ %ux%u В УГЛУ, 3 СЕК\n", PW, PH);
    if (ovl_layer_set(1, ovl_panel, 100, 400, PW, PH, PW * 4, 255)) {
        kprintf("OVL: СЛОЙ 1 НЕ ВСТАЛ\n");
        return;
    }
    delay_ms(3000);

    /*
     * Решающая проверка порядка наложения.
     *
     * Слой 0 — наш основной кадр: во весь экран и непрозрачный. Если он
     * лежит поверх остальных, а не под ними, то всё, что мы кладём на
     * слои 1 и 2, он собой закрывает. Выключаем его на три секунды:
     * появятся наши слои — значит порядок обратный ожидаемому.
     */
    kprintf("OVL: ЭТАП 3 — БЕЗ ОСНОВНОГО КАДРА, 3 СЕК\n");
    ovl_base_layer(0);
    delay_ms(3000);
    ovl_base_layer(1);

    kprintf("OVL: ЭТАП 4 — ДВИЖЕНИЕ\n");
    for (u32 i = 0; i < frames; i++) {
        u32 half = frames / 2;
        u32 k = (i < half) ? i : (frames - 1 - i);
        u32 x = (k * (w - PW)) / half;
        u32 y = (k * (h - PH)) / half;
        u64 t0;

        fb_wait_frame_gap();            /* меняем в окне между кадрами */
        t0 = read_cntpct();
        ovl_layer_move(1, x, y);
        ovl_layer_alpha(1, 128 + (k * 127) / half);
        move_ticks += read_cntpct() - t0;
        usb_poll();
    }

    kprintf("OVL: %u КАДРОВ ДВИЖЕНИЯ, РАБОТЫ НА КАДР %lu МКС\n",
            frames, ticks_to_us(move_ticks / frames));
    kprintf("OVL: ПАНЕЛЬ ПОСЛЕ ДВИЖЕНИЯ  УГОЛ %08x СЕРЕДИНА %08x\n",
            ovl_panel[0], ovl_panel[mid]);

    ovl_layer_off(1);
    kprintf("OVL: СЛОИ УБРАНЫ, СЛОЙ ЗАГРУЗЧИКА ОСТАЁТСЯ ВЫКЛЮЧЕННЫМ\n");
#endif
}

static void fb_flip_demo(void)
{
    const u32 frames = 90;
    const u32 BG = 0xFF101840;          /* тёмно-синий фон демонстрации */
    u64 base; u32 w, h, stride;
    u64 flip_ticks = 0;

    if (!fb_available())
        return;

    if (fb_double_buffer(1) != 0) {
        kprintf("ЭКРАН    : ВТОРОГО БУФЕРА НЕТ (МАЛО ВИДЕОПАМЯТИ)\n");
        return;
    }

    fb_info(&base, &w, &h, &stride);

    /* Первый кадр красим целиком, дальше — только изменившееся.
     *
     * Стирать весь экран каждый кадр слишком дорого: 10 МБ записи в
     * НЕкэшируемую память идут напрямую в DRAM и стоят десятки миллисекунд.
     * Полоса занимает двенадцатую часть экрана, поэтому достаточно затереть
     * её прошлое место. Из-за двойной буферизации затирать надо позицию не
     * предыдущего кадра, а позапрошлого: в буфере, куда мы вернулись после
     * flip, лежит именно он.
     */
    u32 bar = h / 12;
    u32 hist[2] = { 0, 0 };
    u64 draw_ticks = 0;

    fb_clear(BG);
    fb_flip();
    fb_clear(BG);

    for (u32 i = 0; i < frames; i++) {
        u32 y = (i * (h - bar)) / (frames - 1);
        u64 t0 = read_cntpct();
        u64 t1;

        if (i >= 2)
            fb_fill_rect(0, hist[i & 1], w, bar, BG);
        fb_fill_rect(0, y, w, bar, COLOR_GREEN);
        fb_fill_rect(0, y + bar / 3, w, bar / 3, COLOR_CYAN);
        hist[i & 1] = y;

        t1 = read_cntpct();
        draw_ticks += t1 - t0;
        fb_flip();
        flip_ticks += read_cntpct() - t1;

        /* Темп больше не держим по счётчику.
         *
         * Раньше здесь была выдержка до следующей шестидесятой доли
         * секунды, потому что показ кадра возвращался сразу и движение
         * выходило рваным. Теперь fb_flip ждёт оверлей, а он работает
         * ровно шестьдесят раз в секунду — счётчик тикал бы рядом со
         * своим темпом, и два независимых шестидесятых плыли бы друг
         * относительно друга. Ровный ход даёт железо, а не мы. */
    }

    fb_double_buffer(0);
    fb_clear(COLOR_BLACK);
    kprintf("ЭКРАН    : ДВОЙНАЯ БУФЕРИЗАЦИЯ ОК, %u КАДРОВ\n", frames);
    kprintf("ОТРИСОВКА КАДРА : %lu МКС\n", ticks_to_us(draw_ticks / frames));
    kprintf("ПОКАЗ КАДРА     : %lu МКС\n", ticks_to_us(flip_ticks / frames));
    fb_debug();                         /* тут же счётчики vsync */
}

void kmain(u64 dtb_phys)
{
    u64 slow, fast;
    int irq_ok = 0;

    /* Раньше всего: иначе через несколько секунд железо перезагрузит нас
     * само, и никакая отладка не успеет ничего показать. */
    watchdog_disable();
    HALT_STAGE(1);

    /* Своя per-CPU запись до всего остального: this_cpu() понадобится
     * и таймеру, и GIC, и планировщику. Загрузочное ядро всегда номер 0. */
    percpu_init(0, read_mpidr());

    /* Дерево от загрузчика запоминаем сразу: из него берут адреса
     * и разметку памяти и GIC, и pmm. */
    fdt_set_root(dtb_phys);

    uart_init();
    banner();
    HALT_STAGE(2);                          /* UART поднят */

    /* Замеряем «как было»: MMU выключен, вся память Device, кэшей нет */
    slow = bench_memfill();

    kprintf("MMU      : ВКЛЮЧАЮ...\n");
    mmu_enable();
    kprintf("MMU      : ВКЛЮЧЕН (КЭШИ D+I АКТИВНЫ)\n");

    HALT_STAGE(3);                          /* MMU и кэши живы */
    fast = bench_memfill();

    if (fb_init() == 0) {
        u64 base; u32 w, h, stride;

        /* Фреймбуфер обязан быть некэшируемым: контроллер дисплея
         * читает DRAM напрямую и о кэшах процессора не знает. */
        fb_info(&base, &w, &h, &stride);
        mmu_set_range_nc(base, (u64)stride * h * 4);

        fb_set_colors(COLOR_GREEN, COLOR_BLACK);
        fb_clear(COLOR_BLACK);
        banner();
    }

    /*
     * USB-консоль поднимаем как можно раньше: всё, что напечатано после
     * этого момента, попадёт в терминал на компьютере. До сих пор
     * единственным способом прочитать вывод была фотография экрана.
     *
     * Ждём перечисления не дольше трёх секунд: если компьютер не подключён,
     * загрузка не должна из-за этого стоять.
     */
    usb_phy_on();
    usb_connect();
    {
        u64 end = read_cntpct() + read_cntfrq() * 3;

        while (read_cntpct() < end && !usb_ready())
            usb_poll();
    }
    kprintf("USB      : %s\n", usb_ready() ? "КОНСОЛЬ ГОТОВА" : "НЕТ ХОСТА");

    HALT_STAGE(4);                          /* на экране уже должен быть текст */
    dump_cpu();
    dump_dtb(dtb_phys);
    dump_fb();

    kprintf("ПАМЯТЬ БЕЗ КЭША : %lu МКС\n", ticks_to_us(slow));
    kprintf("ПАМЯТЬ С КЭШЕМ  : %lu МКС\n", ticks_to_us(fast));
    if (fast)
        kprintf("УСКОРЕНИЕ       : %lu РАЗ\n", slow / fast);

    test_pattern();

    /* Память. Обязательно ПОСЛЕ fb_init: фреймбуфер тоже лежит в DRAM,
     * его выделил загрузчик, и аллокатор о нём знать не может. */
    memory_setup(dtb_phys);
    HALT_STAGE(5);

    /* Экран берём под себя целиком: слои, оставшиеся от загрузчика,
     * лежат поверх нашего кадра и показывают чужую память. */
    ovl_take_over();

    /* Контроллер заряда: пока только смотрим, что он думает о кабеле
     * и сколько тока разрешил. Заряд — про безопасность батареи, менять
     * что-либо вслепую нельзя. */
    charger_dump();
    if (charger_use_apsd() == 0)
        kprintf("ЗАРЯД: ПРЕДЕЛ ВХОДА ТЕПЕРЬ ОПРЕДЕЛЯЕТ САМ КОНТРОЛЛЕР\n");
    else
        kprintf("ЗАРЯД: ПЕРЕКЛЮЧИТЬ НА РАСПОЗНАВАНИЕ НЕ ВЫШЛО\n");
    charger_dump();

    /* Диагностика экрана печатается ДО демонстрации: после неё экран уже
     * может быть испорчен, и прочитать с него числа не выйдет. */
    fb_debug();
    fb_vsync_probe();
    HALT_STAGE(8);

    /* Тачскрин. Раньше это был отладочный этап, доступный только через
     * HALT_AT — теперь вывод уходит в консоль по USB, и держать разговор
     * с контроллером в стороне от обычной загрузки больше незачем. */
    touch_power_on_only();
    spi_clk_enable();
    spi_pins_setup();
    nvt_probe();
    /* Опрос касаний больше не занимает загрузку: он стал задачей
     * планировщика, а этот показ оставлен только для отладки. */
    if (HALT_AT_OR_ZERO)
        touch_demo(3);

    if (10 == HALT_AT_OR_ZERO)
        fb_flip_probe();
    HALT_STAGE(10);

    if (11 == HALT_AT_OR_ZERO)
        fb_stride_test();
    HALT_STAGE(11);

    if (12 == HALT_AT_OR_ZERO)
        fb_banding_test();
    HALT_STAGE(12);

    if (13 == HALT_AT_OR_ZERO) {
        fb_clear(COLOR_BLACK);
        i2c0_dump();
    }
    HALT_STAGE(13);

    if (14 == HALT_AT_OR_ZERO) {
        fb_clear(COLOR_BLACK);
        i2c0_scan();
    }
    HALT_STAGE(14);

    if (15 == HALT_AT_OR_ZERO) {
        fb_clear(COLOR_BLACK);
        gpio_check();
    }
    HALT_STAGE(15);

    if (16 == HALT_AT_OR_ZERO) {
        fb_clear(COLOR_BLACK);
        gpio_check();
        touch_wake();
    }
    HALT_STAGE(16);

    if (17 == HALT_AT_OR_ZERO) {
        fb_clear(COLOR_BLACK);
        pmic_probe();
    }
    HALT_STAGE(17);

    if (20 == HALT_AT_OR_ZERO) {
        pmic_probe();
        touch_power_on();
    }
    HALT_STAGE(20);

    if (21 == HALT_AT_OR_ZERO) {
        touch_power_on_only();
        spi_clk_enable();
        spi_alive_test();
        spi_pins_setup();
        nvt_probe();
    }
    HALT_STAGE(21);

    if (18 == HALT_AT_OR_ZERO) {
        fb_clear(COLOR_BLACK);
        usb_probe();
    }
    HALT_STAGE(18);

    if (19 == HALT_AT_OR_ZERO) {
        fb_clear(COLOR_BLACK);
        usb_probe();
        usb_phy_on();
        usb_connect();

        /*
         * Теперь не просто смотрим на шину, а отвечаем. Перечисление —
         * это разговор: хост спрашивает дескрипторы, назначает адрес,
         * выбирает конфигурацию, и на каждый шаг ждёт ответа. Молчание
         * дольше отведённого времени он считает неисправностью.
         */
        {
            u64 end = read_cntpct() + read_cntfrq() * 25;
            u32 last = 0;

            kprintf("USB: ОБСЛУЖИВАЮ ХОСТА...\n");
            while (read_cntpct() < end) {
                usb_poll();
                if (usb_ready() && !last) {
                    last = 1;
                    kprintf("USB: ПЕРЕЧИСЛЕНЫ! ЗАПРОСОВ %u\n",
                            usb_setup_count);
                    usb_send((const u8 *)"VELO-OS: USB console alive\n", 28);
                }
            }
            kprintf("USB: ИТОГ — ЗАПРОСОВ %u, %s\n", usb_setup_count,
                    usb_ready() ? "ПЕРЕЧИСЛЕНЫ" : "НЕ ПЕРЕЧИСЛЕНЫ");
#if defined(BOARD_MERLIN)
            kprintf("USB: FADDR %x POWER %x\n",
                    mmio_read8(MT_USB0_BASE + 0x00),
                    mmio_read8(MT_USB0_BASE + 0x01));
#endif
        }
    }
    HALT_STAGE(19);                          /* замереть на чистом экране */

    fb_flip_demo();
    kprint_to_fb(0);     /* кадр нужен под слои, а не под текст */
    /* Показ слоёв тоже под отладку: в обычной работе слои заняты
     * оболочкой, и две демонстрации мешали бы друг другу. */
    if (HALT_AT_OR_ZERO)
        ovl_demo();
    kprint_to_fb(1);
    HALT_STAGE(9);                          /* замереть после демонстрации */

    /* Прерывания. Порядок жёсткий: сперва контроллер, потом таймер
     * (он прописывает себя в контроллер), и только в самом конце снимаем
     * маску DAIF.I. Снять её раньше — поймать прерывание без обработчика. */
    if (gic_init() == 0 && timer_init(TIMER_HZ) == 0) {
        irq_enable();
        irq_ok = irq_works();

        /*
         * Не вышло — пробуем ещё раз, а не сдаёмся.
         *
         * Примерно раз на десяток загрузок ни одного тика за четверть
         * секунды не приходило, и ядро честно уходило в одноядерный режим
         * без задач. Причина плавающая, поэтому сначала печатаем состояние
         * контроллера — по нему видно, не блокирует ли нас чужое
         * прерывание, оставленное загрузчиком незакрытым, — а потом
         * поднимаем контроллер и таймер заново.
         */
        for (u32 tries = 0; !irq_ok && tries < 3; tries++) {
            kprintf("IRQ      : ТИКОВ НЕТ, ПОПЫТКА %u\n", tries + 1);
            gic_debug_dump();
            irq_disable();
            gic_init_cpu();
            timer_init_cpu();
            irq_enable();
            irq_ok = irq_works();
        }
        if (irq_ok)
            gic_debug_dump();
    }
    HALT_STAGE(6);                          /* GIC и таймер */

    /*
     * Без прерываний многозадачности не бывает: вытеснять задачу нечем,
     * и первая же запущенная захватит ядро навсегда. В таком случае
     * не делаем вид, что всё хорошо, а честно остаёмся одноядерными
     * и печатаем пульс по опросу счётчика.
     */
    if (!irq_ok) {
        kprintf("IRQ      : НЕ РАБОТАЮТ, ЗАДАЧИ НЕ ЗАПУСКАЮ\n");
        kprintf("\nBOOT OK. HEARTBEAT:\n");
        heartbeat_polling();
    }

    kprintf("IRQ      : РАЗРЕШЕНЫ\n");

    /* Многоядерность. Ядра будим только после того, как поднята вся
     * общая инфраструктура: разбуженное ядро сразу пойдёт выделять память
     * под свою idle-задачу и включать себе таймер. */
    sched_init();
    sched_init_cpu();
    smp_start_secondaries();
    smp_dump();
    HALT_STAGE(7);                          /* восемь ядер подняты */

    /*
     * Остаток загрузки — обычная задача, а не холостой контекст.
     *
     * Здесь ядро едва не потеряло само себя. После sched_init_cpu() эта
     * функция продолжает исполняться в контексте холостой задачи CPU0, а
     * холостая задача по построению выбирается только тогда, когда
     * очередь готовых пуста — так и написано в планировщике. Стоило
     * завести десяток счётных задач, и очередь перестала пустеть: конец
     * загрузки продвигался рывками, в редкие мгновения общего сна, а
     * иногда не продвигался вовсе.
     *
     * Снаружи это выглядело как зависание на ровном месте: программы не
     * запускались, оболочка не поднималась, при этом система работала и
     * печатала тики. И «чинилось» от любой мелочи, меняющей тайминг, —
     * добавь печать, и всё вдруг успевает.
     *
     * Задача конкурирует за процессор наравне со всеми, и ничего этого
     * больше не может случиться.
     */
    task_create("загрузка", boot_task, NULL);

    /* CPU0 дальше живёт как все: раздаёт себя очереди задач.
     * Пульс теперь такая же задача, поэтому печать не зависит от того,
     * на каком ядре она окажется. */
    sched_idle_loop();
}

/* --- Демонстрационные задачи ---
 *
 * Каждая крутит один и тот же цикл и увеличивает два общих счётчика:
 * один под спинлоком, второй — обычным ++. На одном ядре оба всегда
 * совпадают, и разницу между ними невозможно ни увидеть, ни объяснить.
 * На восьми ядрах незащищённый счётчик начинает терять прибавления
 * прямо на глазах — это и есть самая наглядная причина, зачем нужны
 * блокировки.
 */
static struct spinlock counter_lock = SPINLOCK_INIT("counter");
static u64 counter_locked;
static u64 counter_racy;

static void worker_task(void *arg)
{
    struct worker *w = (struct worker *)arg;

    for (;;) {
        for (u32 i = 0; i < 1000; i++) {
            u64 flags = spin_lock_irq(&counter_lock);

            counter_locked++;
            /* Личный счётчик задачи увеличиваем В ТОМ ЖЕ критическом
             * участке. Сам по себе он в защите не нуждается, но так
             * сверка счётчиков видит согласованный снимок — см. counters_agree */
            w->count++;
            spin_unlock_irq(&counter_lock, flags);

            /* А так делать нельзя — и ниже видно, почему */
            counter_racy++;
        }

        w->rounds++;
        w->cpu = cpu_id();

        /*
         * Пауза между кругами.
         *
         * Эти задачи заведены, чтобы показать планировщик и работу
         * замков, а не чтобы греть процессор. Без паузы десяток таких
         * задач держит оба ядра под сто процентов постоянно — а телефон
         * при этом работает от батареи, и потребление выходит больше,
         * чем даёт зарядка через USB. Круга в тысячу итераций раз в
         * пятьдесят миллисекунд для проверки согласованности счётчиков
         * достаточно с избытком.
         */
        task_sleep_ms(50);
    }
}

/* Проверка, что весь блок заполнен ожидаемым байтом */
static int check_fill(const u8 *p, u8 v, u64 n)
{
    for (u64 i = 0; i < n; i++)
        if (p[i] != v)
            return 0;

    return 1;
}

/*
 * Самопроверка памяти.
 *
 * Аллокатор — это код, чьи ошибки проявляются не там, где сделаны:
 * блок, выданный дважды, всплывёт случайной порчей чужих данных через
 * тысячу операций. Поэтому проверяем сразу и на месте, а результат
 * печатаем — на телефоне это будет единственный доступный отчёт.
 */
static void mem_selftest(void)
{
    u64 total, used, grown_before, grown_after, heap_used, blocks;
    u8 *page, *a, *b, *c, *d;
    int ok = 1;

    pmm_stats(&total, &used);
    kprintf("ПАМЯТЬ   : %lu МБ ВСЕГО, %lu МБ СВОБОДНО\n",
            (total * PAGE_SIZE) / (1024 * 1024),
            ((total - used) * PAGE_SIZE) / (1024 * 1024));

    /* 1. Страница из pmm: должна прийти обнулённой и держать запись */
    page = pmm_alloc();
    if (!page || !check_fill(page, 0, PAGE_SIZE)) {
        kprintf("ТЕСТ PMM : СТРАНИЦА НЕ ВЫДАНА ИЛИ НЕ ОБНУЛЕНА\n");
        ok = 0;
    } else {
        memset(page, 0xA5, PAGE_SIZE);
        if (!check_fill(page, 0xA5, PAGE_SIZE)) {
            kprintf("ТЕСТ PMM : СТРАНИЦА НЕ ДЕРЖИТ ЗАПИСЬ\n");
            ok = 0;
        }
        pmm_free(page);
    }

    /* 2. Три соседних блока кучи не должны залезать друг на друга.
     *    Сначала пишем все три, потом проверяем все три: перекрытие
     *    заметно только так — последняя запись затрёт чужие данные. */
    a = kmalloc(512);
    b = kmalloc(512);
    c = kmalloc(512);
    if (!a || !b || !c) {
        kprintf("ТЕСТ КУЧИ: БЛОКИ НЕ ВЫДЕЛИЛИСЬ\n");
        ok = 0;
    } else {
        memset(a, 0x11, 512);
        memset(b, 0x22, 512);
        memset(c, 0x33, 512);

        if (!check_fill(a, 0x11, 512) ||
            !check_fill(b, 0x22, 512) ||
            !check_fill(c, 0x33, 512)) {
            kprintf("ТЕСТ КУЧИ: БЛОКИ ПЕРЕКРЫВАЮТСЯ\n");
            ok = 0;
        }

        /* 3. Склейка. Освобождаем два соседних блока и просим кусок,
         *    который заведомо больше любого свободного блока по
         *    отдельности. Если куча при этом не пошла к pmm за новыми
         *    страницами — значит соседи действительно склеились. */
        heap_stats(&grown_before, NULL, NULL);
        kfree(b);
        kfree(c);

        d = kmalloc(3000);
        heap_stats(&grown_after, NULL, NULL);

        if (!d) {
            kprintf("ТЕСТ КУЧИ: НЕТ БЛОКА ПОСЛЕ СКЛЕЙКИ\n");
            ok = 0;
        } else if (grown_after != grown_before) {
            kprintf("ТЕСТ КУЧИ: СКЛЕЙКИ НЕ ПРОИЗОШЛО, КУЧА ВЫРОСЛА\n");
            ok = 0;
        }

        kfree(d);
        kfree(a);
    }

    /* 4. Нагрузка. Один цикл «выделил — освободил» проходит и на кривом
     *    аллокаторе; ошибки в дроблении и склейке вылезают только когда
     *    блоки разного размера перемешаны и освобождаются вразнобой.
     *    Каждый блок помечаем своим байтом и проверяем ВСЕ в конце —
     *    так видно, если два указателя показали на одну память. */
    {
        u8 *blocks_arr[32];
        int stress_ok = 1;

        for (u32 i = 0; i < ARRAY_SIZE(blocks_arr); i++) {
            u64 sz = 16 + (i * 37) % 700;       /* размеры вразнобой */

            blocks_arr[i] = kmalloc(sz);
            if (!blocks_arr[i]) {
                stress_ok = 0;
                break;
            }
            memset(blocks_arr[i], (u8)(i + 1), sz);
        }

        /* Освобождаем каждый второй — куча становится «дырявой» */
        for (u32 i = 0; i < ARRAY_SIZE(blocks_arr); i += 2) {
            if (blocks_arr[i]) {
                kfree(blocks_arr[i]);
                blocks_arr[i] = NULL;
            }
        }

        /* Оставшиеся обязаны пережить и освобождение соседей, и склейку */
        for (u32 i = 1; i < ARRAY_SIZE(blocks_arr); i += 2) {
            u64 sz = 16 + (i * 37) % 700;

            if (blocks_arr[i] && !check_fill(blocks_arr[i], (u8)(i + 1), sz))
                stress_ok = 0;
        }

        for (u32 i = 0; i < ARRAY_SIZE(blocks_arr); i++)
            kfree(blocks_arr[i]);

        if (!stress_ok) {
            kprintf("ТЕСТ КУЧИ: НАГРУЗКА ИСПОРТИЛА ДАННЫЕ\n");
            ok = 0;
        }
    }

    /* 5. После всех освобождений занятых байт быть не должно */
    heap_stats(&grown_after, &heap_used, &blocks);
    if (heap_used != 0) {
        kprintf("ТЕСТ КУЧИ: УТЕЧКА %lu БАЙТ\n", heap_used);
        ok = 0;
    }

    kprintf("КУЧА     : %lu КБ У PMM, БЛОКОВ %lu, ЗАНЯТО %lu\n",
            grown_after / 1024, blocks, heap_used);
    kprintf("ТЕСТ     : %s\n", ok ? "ПАМЯТЬ РАБОТАЕТ" : "ЕСТЬ ОШИБКИ (СМ. ВЫШЕ)");
}

/*
 * Дошло ли до нас хоть одно прерывание таймера.
 *
 * Проверять это ОБЯЗАТЕЛЬНО активным ожиданием, а не через wfi: если
 * прерывания не доходят, будить процессор из wfi будет нечему, и он
 * останется там навсегда. На телефоне это выглядело бы как чёрный экран
 * без единого слова о причине — ровно то, чего мы избегаем.
 */
static int irq_works(void)
{
    u64 deadline = read_cntvct() + read_cntfrq() / 4;   /* даём 250 мс */

    while (read_cntvct() < deadline)
        if (timer_ticks() > 0)
            return 1;

    return 0;
}

/*
 * Пульс ядра. Раньше это был цикл активного ожидания; теперь процессор стоит
 * в wfi и просыпается только по прерыванию таймера. Заодно это и проверка
 * всей цепочки: таймер -> GIC -> вектор -> обработчик -> EOI.
 */
/*
 * Сходится ли общий счётчик с суммой личных.
 *
 * Каждая задача увеличивает общий счётчик и свой собственный в одном и том же
 * критическом участке. Поэтому, удерживая тот же замок, мы видим момент, когда
 * ни одна задача не находится между двумя прибавлениями, — и сумма личных
 * обязана совпасть с общим ТОЧНО, без всяких допусков.
 *
 * Сначала здесь стоял допуск «на пару задач», а снимок брался без замка:
 * общий счётчик читался раньше личных, и пока мы складывали десять чисел,
 * восемь ядер успевали накрутить ещё сотни. Проверка изредка ругалась на
 * ровном месте — то есть проверяла скорость чтения, а не работу замка.
 */
static int counters_agree(void)
{
    u64 sum = 0;
    u64 flags = spin_lock_irq(&counter_lock);
    u64 total = counter_locked;
    int ok;

    for (u32 i = 0; i < ARRAY_SIZE(workers); i++)
        sum += workers[i].count;

    ok = (total == sum);
    spin_unlock_irq(&counter_lock, flags);

    return ok;
}

/* Мигание квадратом в углу — чтобы «живо» было видно и без текста */
static void heartbeat_blink(u32 beat)
{
    if (fb_available())
        fb_fill_rect(0, 0, 40, 40, (beat & 1) ? COLOR_GREEN : COLOR_BLACK);
}

/*
 * Пульс как обычная задача.
 *
 * Раньше это был цикл в контексте загрузки, и на многоядерной системе он бы
 * голодал: ядро, занятое бесконечной задачей, до печати уже не дошло бы.
 * Задача же участвует в общей карусели наравне со всеми — и заодно служит
 * доказательством, что планировщик действительно возвращает управление.
 */
static void heartbeat_task(void *arg)
{
    u64 last_sec = 0;
    u32 beat = 0;

    (void)arg;

    for (;;) {
        u64 sec = timer_uptime_ms() / 1000;

        if (sec == last_sec) {
            wfi();          /* до ближайшего прерывания делать нечего */
            continue;
        }
        last_sec = sec;
        beat++;

        /* Раз в десять секунд, а не каждую.
         *
         * Кольцо вывода в консоль — шестнадцать килобайт, и посекундный
         * отчёт забивал его за минуту: любой загрузочный вывод оказывался
         * вытеснен ещё до того, как консоль успевала подключиться.
         * Диагностировать в таких условиях невозможно, а пульс раз в
         * десять секунд говорит ровно то же самое. */
        if (beat % 10)
            continue;

        /* Потери печатаем абсолютным числом, а не процентом: доля выходит
         * меньше процента и в целых числах всегда показывала бы ноль,
         * то есть ровно скрывала бы то, ради чего счётчик и заведён. */
        kprintf("TICK %lu  IRQ %lu (НИЧЕЙНЫХ %lu)  ЯДЕР %u  ЗАДАЧ %u\n",
                sec, gic_count(), gic_spurious(),
                cpu_online_count(), sched_task_count());
        kprintf("         СЧЁТ %lu  ПОТЕРЯНО БЕЗ ЗАМКА %lu  СВЕРКА %s\n",
                counter_locked, counter_locked - counter_racy,
                counters_agree() ? "ОК" : "РАСХОЖДЕНИЕ");
        kprintf("         ПРОГРАММ ВЫТЕСНЕНО ПРЯМО В EL0: %lu\n",
                el0_preempt_count());

        kprintf("         ВВОД: ОПРОСОВ %lu, СБОЕВ %lu, СОБЫТИЙ %lu, ПОТЕРЯНО %u\n",
                input_scans, input_fails, input_events, input_dropped);

        /* Кто-то застрял в запуске программы — скажем, на каком шаге.
         * Печатаем отсюда, а не оттуда: печать по шагам сдвигает
         * тайминг, и зависание перестаёт воспроизводиться. */
        if (uspace_stage)
            kprintf("         ЗАПУСК %s ЗАСТРЯЛ НА ШАГЕ %u\n",
                    uspace_who ? uspace_who : "?", uspace_stage);

        if (beat % 50 == 0)
            sched_dump();

        heartbeat_blink(beat);
    }
}

/*
 * Конец загрузки: задачи, программы, ввод и оболочка.
 *
 * Вынесено из kmain в отдельную задачу по причине, описанной в месте
 * вызова: холостой контекст, в котором kmain оказывается после запуска
 * планировщика, получает процессор только когда заняться больше нечем.
 */
static void boot_task(void *arg)
{
    (void)arg;

    /* Задачи. Их подхватит любое свободное ядро — очередь одна на всех. */
    for (u32 i = 0; i < ARRAY_SIZE(workers); i++) {
        workers[i].index = i;
        task_create(worker_names[i], worker_task, &workers[i]);
    }
    task_create("пульс", heartbeat_task, NULL);

    /*
     * Программы в пользовательском режиме.
     *
     * Здороваются, считают, проверяют раздельность пространств и
     * запускают друг друга. Ядро заводит их по номерам из общего с EL0
     * списка — того же, которым пользуется системный вызов SYS_SPAWN.
     */
    uspace_spawn_image(IMG_HELLO);
    uspace_spawn_image(IMG_SPIN);

    /* Двойники: одна и та же программа по одному и тому же адресу в двух
     * пространствах. Если раздельность где-то сломана, они это заметят
     * друг о друге раньше, чем мы — по дескрипторам. */
    uspace_spawn_image(IMG_TWIN);
    uspace_spawn_image(IMG_TWIN);

    /* Запускала сама заведёт и обычную программу, и нарушителя, и
     * дождётся обоих: отдельно нарушителя отсюда запускать больше не
     * нужно. */
    uspace_spawn_image(IMG_BOSS);

    /*
     * Художник, следопыт и панель на загрузке больше не запускаются.
     *
     * Слоёв под окна два, а оболочке нужен свой и на весь экран.
     * Программы никуда не делись — они в образе, и запустить их может
     * кто угодно вызовом spawn; своё дело (рисование, ввод, текст) они
     * уже доказали, и теперь то же самое делает оболочка.
     */
    /* Первая программа на Си: проверяет путь сборки целиком */
    uspace_spawn_image(IMG_HELLO_C);

    /* Оболочка — теперь программа. Ядро интерфейсом больше не занято. */
    uspace_spawn_image(IMG_SHELL);

    /*
     * eMMC: только смотрим, что оставил загрузчик. Ни одной записи —
     * это единственное место, где наша ошибка необратима.
     *
     * Печатается в самом конце загрузки намеренно. Кольцо консоли — 16
     * килобайт, а терминал на компьютере открывает порт через пару
     * секунд после старта: всё, что напечатано раньше, к этому моменту
     * успевает вытесниться. Дамп на два десятка строк ровно так и
     * пропал в первый раз.
     */
    emmc_probe();

    /*
     * Экран отдан оболочке, поэтому отладочный вывод с него убираем:
     * поверх её кадра он всё равно не виден, а под ним только мешает.
     * В консоль по USB он идёт по-прежнему.
     */
    kprint_to_fb(0);

    /* Сборщик и проверка того, что после него память возвращается */
    sched_start_reaper();
    task_create("перезапуск", respawn_task, NULL);

    /* Ввод и оболочка. Порядок важен: оболочка сразу забирает экран у
     * отладочного вывода, поэтому запускаем её последней — всё, что
     * печаталось до этого, успевает лечь на экран и остаётся видимым,
     * пока она не нарисует первый кадр. */
    input_start();
    ui_start();

    kprintf("\nBOOT OK. ЗАДАЧИ ПОШЛИ:\n");
}

/*
 * Проверка того, что память возвращается.
 *
 * Запускает десяток программ подряд и сравнивает занятость памяти до и
 * после. Без сборщика каждая программа уносит с собой девять страниц —
 * код, стек, три таблицы, корень — и число после заметно больше числа
 * до. Со сборщиком они совпадают.
 *
 * Пауза в конце нужна не для красоты: сборщик просыпается раз в
 * четверть секунды, и мерить сразу означало бы мерить его сон.
 */
#define RESPAWN_COUNT   12

static void respawn_task(void *arg)
{
    u64 total, before, after;

    (void)arg;

    task_sleep_ms(4000);        /* дать отработать первым программам */
    pmm_stats(&total, &before);
    kprintf("EL0      : ЗАПУСКАЮ %u ПРОГРАММ ПОДРЯД, ЗАНЯТО СТРАНИЦ %lu\n",
            RESPAWN_COUNT, before);

    for (u32 i = 0; i < RESPAWN_COUNT; i++) {
        uspace_spawn_image(IMG_ONCE);
        task_sleep_ms(150);
    }

    task_sleep_ms(1500);
    pmm_stats(&total, &after);
    kprintf("EL0      : ОТРАБОТАЛИ ВСЕ. ЗАНЯТО СТРАНИЦ %lu, БЫЛО %lu\n",
            after, before);
}

/* Запасной пульс: прерываний нет, значит нет ни задач, ни сна — только опрос */
static void heartbeat_polling(void)
{
    u32 beat = 0;

    for (;;) {
        delay_ms(1000);
        beat++;
        kprintf("TICK %u  UPTIME %lu МС (БЕЗ ПРЕРЫВАНИЙ)\n",
                beat, timer_uptime_ms());
        heartbeat_blink(beat);
    }
}

/* --- Обработчики из vectors.S --- */

void exception_fatal(u64 type, u64 esr, u64 elr, u64 far)
{
    static const char *names[16] = {
        "SYNC SP0", "IRQ SP0", "FIQ SP0", "SERR SP0",
        "SYNC",     "IRQ",     "FIQ",     "SERROR",
        "SYNC L64", "IRQ L64", "FIQ L64", "SERR L64",
        "SYNC L32", "IRQ L32", "FIQ L32", "SERR L32",
    };

    fb_set_colors(COLOR_RED, COLOR_BLACK);
    kprintf("\n\n*** PANIC: %s ***\n", names[type & 15]);
    kprintf("ESR  = %016lx  (EC=%lu)\n", esr, (esr >> 26) & 0x3F);
    kprintf("ELR  = %016lx\n", elr);
    kprintf("FAR  = %016lx\n", far);
    kprintf("HALTED.\n");

    for (;;)
        wfi();
}

void irq_handler(void)
{
    gic_dispatch();

    /*
     * Вытеснение делается ЗДЕСЬ, а не внутри обработчика таймера: к этому
     * моменту прерывание уже подтверждено и закрыто через EOI. Переключись
     * мы раньше, незакрытое прерывание осталось бы активным на этом ядре
     * и блокировало бы все следующие того же приоритета.
     */
    if (this_cpu()->resched) {
        this_cpu()->resched = 0;
        schedule();
    }
}
