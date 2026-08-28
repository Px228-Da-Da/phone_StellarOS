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

static void heartbeat_task(void *arg);
static void heartbeat_polling(void);
static int  irq_works(void);
static void mem_selftest(void);
static void worker_task(void *arg);

/* Состояние демонстрационной задачи: у каждой своё, общего — только счётчики */
struct worker {
    u32 index;
    u32 cpu;                    /* на каком ядре отработала последний круг */
    u64 rounds;
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
        ;
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

void kmain(u64 dtb_phys)
{
    u64 slow, fast;
    int irq_ok = 0;

    /* Своя per-CPU запись до всего остального: this_cpu() понадобится
     * и таймеру, и GIC, и планировщику. Загрузочное ядро всегда номер 0. */
    percpu_init(0, read_mpidr());

    uart_init();
    banner();

    /* Замеряем «как было»: MMU выключен, вся память Device, кэшей нет */
    slow = bench_memfill();

    kprintf("MMU      : ВКЛЮЧАЮ...\n");
    mmu_enable();
    kprintf("MMU      : ВКЛЮЧЕН (КЭШИ D+I АКТИВНЫ)\n");

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

    dump_cpu();
    dump_dtb(dtb_phys);
    dump_fb();

    kprintf("ПАМЯТЬ БЕЗ КЭША : %lu МКС\n", ticks_to_us(slow));
    kprintf("ПАМЯТЬ С КЭШЕМ  : %lu МКС\n", ticks_to_us(fast));
    if (fast)
        kprintf("УСКОРЕНИЕ       : %lu РАЗ\n", slow / fast);

    test_pattern();

    /* Память. Обязательно ПОСЛЕ fb_init: фреймбуфер тоже лежит в DRAM,
     * его выделил загрузчик, и аллокатор о нём знать не может. Не пометить
     * его занятым — значит однажды выдать его под кучу и получить кашу
     * на экране вместо картинки. */
    if (pmm_init(RAM_BASE, RAM_SIZE, dtb_phys) == 0) {
        if (fb_available()) {
            u64 base; u32 w, h, stride;

            fb_info(&base, &w, &h, &stride);
            pmm_reserve(base, (u64)stride * h * 4);
        }
        mem_selftest();
    }

    /* Прерывания. Порядок жёсткий: сперва контроллер, потом таймер
     * (он прописывает себя в контроллер), и только в самом конце снимаем
     * маску DAIF.I. Снять её раньше — поймать прерывание без обработчика. */
    if (gic_init() == 0 && timer_init(TIMER_HZ) == 0) {
        irq_enable();
        irq_ok = irq_works();
    }

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

    /* Задачи. Их подхватит любое свободное ядро — очередь одна на всех. */
    for (u32 i = 0; i < ARRAY_SIZE(workers); i++) {
        workers[i].index = i;
        task_create(worker_names[i], worker_task, &workers[i]);
    }
    task_create("пульс", heartbeat_task, NULL);

    kprintf("\nBOOT OK. ЗАДАЧИ ПОШЛИ:\n");

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
            spin_unlock_irq(&counter_lock, flags);

            /* А так делать нельзя — и ниже видно, почему */
            counter_racy++;
        }

        w->rounds++;
        w->cpu = cpu_id();
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

        /* Потери печатаем абсолютным числом, а не процентом: доля выходит
         * меньше процента и в целых числах всегда показывала бы ноль,
         * то есть ровно скрывала бы то, ради чего счётчик и заведён. */
        kprintf("TICK %lu  IRQ %lu  ЯДЕР %u  ЗАДАЧ %u  СЧЁТ %lu  ПОТЕРЯНО БЕЗ ЗАМКА %lu\n",
                sec, gic_count(), cpu_online_count(), sched_task_count(),
                counter_locked, counter_locked - counter_racy);

        if (beat % 5 == 0)
            sched_dump();

        heartbeat_blink(beat);
    }
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
