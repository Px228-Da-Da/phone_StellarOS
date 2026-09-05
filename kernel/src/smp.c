/*
 * smp.c — запуск и учёт ядер.
 *
 * Порядок оживления вторичного ядра:
 *
 *   1. CPU0 просит прошивку: psci_cpu_on(mpidr, _secondary_entry, номер)
 *   2. Прошивка включает питание ядра и передаёт ему управление
 *   3. Ядро приходит в _secondary_entry с ВЫКЛЮЧЕННЫМ MMU и своим стеком
 *   4. secondary_main: своя запись в TPIDR_EL1, MMU, GIC, таймер
 *   5. online = 1 — CPU0 видит, что ядро поднялось
 *
 * Пункт 4 нельзя переставлять. Пока MMU выключен, память для этого ядра
 * некэшируемая, и данные, записанные CPU0 в кэш, ему не видны: он прочитает
 * из DRAM то, что было там до этого. Поэтому до включения MMU вторичное
 * ядро не трогает НИЧЕГО общего — только свой стек и свои системные регистры.
 */
#include "smp.h"
#include "psci.h"
#include "mmu.h"
#include "gic.h"
#include "timer.h"
#include "sched.h"
#include "print.h"
#include "io.h"
#include "spinlock.h"
#include "fdt.h"

/* Вход для вторичных ядер, boot.S */
extern void _secondary_entry(void);

static struct cpu cpus[MAX_CPUS];
static u64 online_count;

void percpu_init(u32 id, u64 mpidr)
{
    struct cpu *c = &cpus[id];

    c->id = id;
    c->mpidr = mpidr;

    /* TPIDR_EL1 — «свой карман» ядра ОС. Дальше this_cpu() стоит
     * одну инструкцию и работает одинаково на любом ядре. */
    __asm__ volatile("msr tpidr_el1, %0" :: "r"(c) : "memory");
}

struct cpu *this_cpu(void)
{
    struct cpu *c;

    __asm__ volatile("mrs %0, tpidr_el1" : "=r"(c));
    return c;
}

struct cpu *cpu_of(u32 id)
{
    return id < MAX_CPUS ? &cpus[id] : NULL;
}

u32 cpu_id(void)
{
    struct cpu *c = this_cpu();

    return c ? (u32)c->id : 0;
}

u32 cpu_online_count(void)
{
    return (u32)atomic_read(&online_count);
}

/*
 * Точка входа вторичного ядра на Си. Вызывается из boot.S,
 * MMU ещё выключен, из общего состояния трогать нельзя ничего.
 */
void secondary_main(u64 id)
{
    /*
     * MMU включаем ПЕРВЫМ делом, и только потом трогаем общее.
     *
     * Раньше здесь сначала вызывался percpu_init, а он пишет в общий
     * массив cpus[] — то есть нарушал правило, записанное в шапке этого
     * же файла. Пока MMU выключен, запись идёт мимо кэша прямо в память,
     * а CPU0 читает то же место через кэш и видит старое: в отчёте о
     * ядрах появлялся MPIDR 0. Опаснее обратное направление — строка
     * кэша CPU0 могла вытесниться поверх записанного вторым ядром.
     *
     * Ничего общего для включения MMU не нужно: mmu_enable_cpu работает
     * только с системными регистрами.
     */
    mmu_enable_cpu();

    /* С этого момента память кэшируемая и общая с остальными ядрами */
    percpu_init((u32)id, read_mpidr());

    /* Свой интерфейс контроллера прерываний и свой таймер: и то, и другое
     * у каждого ядра собственное, настройки CPU0 сюда не распространяются.
     *
     * Если хоть что-то из этого не поднялось — ядро НЕ идёт в планировщик
     * и не отмечается работающим. Без своего таймера его никто не вытеснит:
     * взятая задача осталась бы на нём навсегда, а в отчёте ядро выглядело
     * бы живым и работающим. Лучше честно потерять ядро, чем задачу. */
    if (gic_init_cpu() != 0 || timer_init_cpu() != 0) {
        kprintf("SMP      : ЯДРО %lu НЕ ГОТОВО, ОСТАНАВЛИВАЮ ЕГО\n", id);
        for (;;)
            wfi();
    }

    sched_init_cpu();

    this_cpu()->online = 1;
    atomic_inc(&online_count);

    irq_enable();
    sched_idle_loop();          /* не возвращается */
}

/*
 * Разбудить остальные ядра.
 *
 * Идентификаторы берём из дерева устройств, узел /cpus. Раньше здесь
 * стоял список, составленный по ИМЕНАМ узлов — cpu@000..cpu@103, — и это
 * была ошибка: имя и содержимое reg у merlin расходятся. Последнее ядро
 * называется cpu@103, а reg у него 0x0700. Настоящие адреса идут через
 * 0x100: 0x000, 0x100, 0x200 и так до 0x700.
 *
 * Из старого списка совпадали ровно два значения, поэтому и поднималось
 * два ядра из восьми. Остальные шесть прошивка отвергала как
 * несуществующие, а мы считали такой ответ штатным и молчали.
 *
 * Запасной список оставлен на случай дерева без /cpus, но теперь он
 * правильный.
 */
u32 smp_start_secondaries(void)
{
    static const u64 fallback[] = {
        0x000, 0x100, 0x200, 0x300, 0x400, 0x500, 0x600, 0x700,
    };
    u64 from_dt[MAX_CPUS];
    const u64 *candidates = fallback;
    u32 candidate_count = ARRAY_SIZE(fallback);
    u32 found = fdt_cpus(fdt_root(), from_dt, MAX_CPUS);
    u64 self = read_mpidr() & 0x00FFFFFFUL;
    s64 ver = psci_version();
    u32 next_id = 1;

    /* Загрузочное ядро тоже входит в счёт: оно работает с самого начала,
     * просто отмечать себя ему было негде до этого момента. */
    if (!cpus[0].online) {
        cpus[0].online = 1;
        atomic_inc(&online_count);
    }

    if (ver < 0) {
        kprintf("SMP      : PSCI НЕДОСТУПЕН, ОСТАЁМСЯ НА ОДНОМ ЯДРЕ\n");
        return 1;
    }

    kprintf("SMP      : PSCI %lu.%lu\n",
            ((u64)ver >> 16) & 0xFFFF, (u64)ver & 0xFFFF);

    if (found) {
        candidates = from_dt;
        candidate_count = found;
        kprintf("SMP      : В ДЕРЕВЕ ЯДЕР %u, ПЕРВОЕ %lx, ПОСЛЕДНЕЕ %lx\n",
                found, from_dt[0], from_dt[found - 1]);
    } else {
        kprintf("SMP      : /cpus В ДЕРЕВЕ НЕТ, БЕРУ ЗАПАСНОЙ СПИСОК\n");
    }

    for (u32 i = 0; i < candidate_count && next_id < MAX_CPUS; i++) {
        u64 mpidr = candidates[i];
        s64 rc;

        if (mpidr == self)
            continue;

        rc = psci_cpu_on(mpidr, (u64)(uintptr_t)_secondary_entry, next_id);
        if (rc != PSCI_SUCCESS) {
            /* Раньше отказ проглатывался молча — и шесть ядер из восьми
             * не поднимались незаметно. Теперь видно, кто и почему. */
            kprintf("SMP      : ЯДРО %lx НЕ ЗАПУСТИЛОСЬ, ОТВЕТ %ld\n",
                    mpidr, rc);
            continue;
        }

        /* Ждём, пока ядро само отчитается. Ограничение по времени
         * обязательно: ядро может не подняться, и висеть здесь вечно
         * означало бы потерять и то ядро, которое работает. */
        u64 deadline = read_cntvct() + read_cntfrq();   /* секунда */

        while (!cpus[next_id].online && read_cntvct() < deadline)
            ;

        if (!cpus[next_id].online) {
            kprintf("SMP      : ЯДРО MPIDR %lx НЕ ОТВЕТИЛО\n", mpidr);
            continue;
        }

        next_id++;
    }

    return cpu_online_count();
}

void smp_dump(void)
{
    kprintf("ЯДЕР     : %u\n", cpu_online_count());

    for (u32 i = 0; i < MAX_CPUS; i++)
        if (cpus[i].online)
            kprintf("  CPU%u   : MPIDR %lx, ТИКОВ %lu\n",
                    i, cpus[i].mpidr, cpus[i].ticks);
}
